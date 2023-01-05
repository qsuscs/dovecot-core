/* Copyright (c) 2013-2018 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "str.h"
#include "str-sanitize.h"
#include "ostream.h"
#include "connection.h"
#include "restrict-access.h"
#include "settings-parser.h"
#include "master-service.h"
#include "master-service-settings.h"
#include "mail-namespace.h"
#include "mail-storage.h"
#include "mail-storage-settings.h"
#include "mail-storage-service.h"
#include "smtp-address.h"
#include "quota-private.h"
#include "quota-plugin.h"
#include "quota-status-settings.h"

enum quota_protocol {
	QUOTA_PROTOCOL_UNKNOWN = 0,
	QUOTA_PROTOCOL_POSTFIX
};

struct quota_client {
	struct connection conn;

	struct event *event;

	char *state;
	char *recipient;
	uoff_t size;

	bool warned_bad_state:1;
};

static struct event_category event_category_quota_status = {
	.name = "quota-status"
};

static const struct quota_status_settings *quota_status_settings;
static enum quota_protocol protocol;
static struct mail_storage_service_ctx *storage_service;
static struct connection_list *clients;
static char *nouser_reply;

static void client_connected(struct master_service_connection *conn)
{
	struct quota_client *client;

	client = i_new(struct quota_client, 1);

	client->event = event_create(NULL);
	client->conn.event_parent = client->event;
	event_add_category(client->event, &event_category_quota_status);
	connection_init_server(clients, &client->conn,
			       "quota-client", conn->fd, conn->fd);
	master_service_client_connection_accept(conn);

	e_debug(client->event, "Client connected");
}

static void client_reset(struct quota_client *client)
{
	i_free(client->state);
	i_free(client->recipient);
}

static enum quota_alloc_result
quota_check(struct mail_user *user, uoff_t mail_size, const char **error_r)
{
	struct quota_user *quser = QUOTA_USER_CONTEXT(user);
	struct mail_namespace *ns;
	struct mailbox *box;
	struct quota_transaction_context *ctx;
	enum quota_alloc_result ret;

	if (quser == NULL) {
		/* no quota for user */
		e_debug(user->event, "User has no quota");
		return QUOTA_ALLOC_RESULT_OK;
	}

	ns = mail_namespace_find_inbox(user->namespaces);
	box = mailbox_alloc(ns->list, "INBOX", MAILBOX_FLAG_POST_SESSION);

	ctx = quota_transaction_begin(box);
	const char *internal_error;
	ret = quota_test_alloc(ctx, I_MAX(1, mail_size), &internal_error);
	if (ret == QUOTA_ALLOC_RESULT_TEMPFAIL)
		e_error(user->event, "quota check failed: %s", internal_error);
	*error_r = quota_alloc_result_errstr(ret, ctx);
	quota_transaction_rollback(&ctx);

	mailbox_free(&box);
	return ret;
}

static int client_check_mta_state(struct quota_client *client)
{
	if (client->state == NULL ||
	    strcasecmp(client->state, "RCPT") == 0 ||
	    strcasecmp(client->state, "END-OF-MESSAGE") == 0)
		return 0;

	if (!client->warned_bad_state) {
		e_warning(client->event,
		          "Received policy query from MTA in unexpected state %s "
		          "(service can only be used for recipient restrictions)",
		          client->state);
	}
	client->warned_bad_state = TRUE;
	return -1;
}

static void client_handle_request(struct quota_client *client)
{
	struct mail_storage_service_input input;
	struct mail_user *user;
	struct smtp_address *rcpt;
	const char *value = NULL, *error;
	const char *detail ATTR_UNUSED;
	char delim ATTR_UNUSED;
	string_t *resp;
	int ret;

	/* this comes in with multiple recipient, and we can reply
	   dunno here. It provides the number of recipients that Postfix
	   accepted for the current message */
	if (client->state != NULL && client->recipient == NULL &&
	    strcasecmp(client->state, "END-OF-MESSAGE") == 0) {
		e_debug(client->event, "Response: action=DUNNO");
		o_stream_nsend_str(client->conn.output, "action=DUNNO\n\n");
		return;
	}

	if (client_check_mta_state(client) < 0 || client->recipient == NULL) {
		e_debug(client->event, "Response: action=DUNNO");
		o_stream_nsend_str(client->conn.output, "action=DUNNO\n\n");
		return;
	}

	if (smtp_address_parse_path(pool_datastack_create(), client->recipient,
				    SMTP_ADDRESS_PARSE_FLAG_ALLOW_LOCALPART |
				    SMTP_ADDRESS_PARSE_FLAG_BRACKETS_OPTIONAL |
				    SMTP_ADDRESS_PARSE_FLAG_ALLOW_BAD_LOCALPART,
				    &rcpt, &error) < 0) {
		e_error(client->event,
			"Client sent invalid recipient address `%s': "
			"%s", str_sanitize(client->recipient, 256), error);
		e_debug(client->event, "Response: action=DUNNO");
		o_stream_nsend_str(client->conn.output, "action=DUNNO\n\n");
		return;
	}

	i_zero(&input);
	input.event_parent = client->event;
	smtp_address_detail_parse_temp(quota_status_settings->recipient_delimiter,
				       rcpt, &input.username, &delim,
				       &detail);
	ret = mail_storage_service_lookup_next(storage_service, &input,
					       &user, &error);
	restrict_access_allow_coredumps(TRUE);
	if (ret == 0) {
		e_debug(client->event, "User `%s' not found", input.username);
		value = nouser_reply;
	} else if (ret > 0) {
		enum quota_alloc_result qret = quota_check(user, client->size,
							   &error);
		if (qret == QUOTA_ALLOC_RESULT_OK) {
			e_debug(client->event,
				"Message is acceptable");
		} else {
			e_debug(client->event,
				"Quota check failed: %s", error);
		}

		switch (qret) {
		case QUOTA_ALLOC_RESULT_OK: /* under quota */
			value = mail_user_plugin_getenv(user,
						"quota_status_success");
			if (value == NULL)
				value = "OK";
			break;
		case QUOTA_ALLOC_RESULT_OVER_MAXSIZE:
		/* even over maximum quota */
		case QUOTA_ALLOC_RESULT_OVER_QUOTA_LIMIT:
			value = mail_user_plugin_getenv(user,
						"quota_status_toolarge");
			/* fall through */
		case QUOTA_ALLOC_RESULT_OVER_QUOTA:
			if (value == NULL)
				value = mail_user_plugin_getenv(user,
						"quota_status_overquota");
			if (value == NULL)
				value = t_strdup_printf("554 5.2.2 %s", error);
			break;
		case QUOTA_ALLOC_RESULT_TEMPFAIL:
		case QUOTA_ALLOC_RESULT_BACKGROUND_CALC:
			ret = -1;
			break;
		}
		value = t_strdup(value); /* user's pool is being freed */
		mail_user_deinit(&user);
	} else {
		e_error(client->event,
			"Failed to lookup user %s: %s", input.username, error);
		error = "Temporary internal error";
	}

	resp = t_str_new(256);
	if (ret < 0) {
		/* temporary failure */
		str_append(resp, "action=DEFER_IF_PERMIT ");
		str_append(resp, error);
	} else {
		str_append(resp, "action=");
		str_append(resp, value);
	}

	e_debug(client->event, "Response: %s", str_c(resp));
	str_append(resp, "\n\n");
	o_stream_nsend_str(client->conn.output, str_c(resp));
}

static int client_input_line(struct connection *conn, const char *line)
{
	struct quota_client *client = (struct quota_client *)conn;
	const char *value;

	e_debug(client->event, "Request: %s", str_sanitize(line, 1024));

	if (*line == '\0') {
		o_stream_cork(conn->output);
		client_handle_request(client);
		o_stream_uncork(conn->output);
		client_reset(client);
		return 1;
	}
	if (str_begins(line, "recipient=", &value)) {
		if (client->recipient == NULL)
			client->recipient = i_strdup_empty(value);
	} else if (str_begins(line, "size=", &value)) {
		if (str_to_uoff(value, &client->size) < 0)
			client->size = 0;
	} else if (str_begins(line, "protocol_state=", &value)) {
		if (client->state == NULL)
			client->state = i_strdup(value);
	}
	return 1;
}

static void client_destroy(struct connection *conn)
{
	struct quota_client *client = (struct quota_client *)conn;

	e_debug(client->event, "Client disconnected");

	connection_deinit(&client->conn);
	client_reset(client);
	event_unref(&client->event);
	i_free(client);

	master_service_client_connection_destroyed(master_service);
}

static struct connection_settings client_set = {
	.input_max_size = SIZE_MAX,
	.output_max_size = SIZE_MAX,
	.client = FALSE
};

static const struct connection_vfuncs client_vfuncs = {
	.destroy = client_destroy,
	.input_line = client_input_line
};

static void main_preinit(void)
{
	restrict_access_by_env(RESTRICT_ACCESS_FLAG_ALLOW_ROOT, NULL);
	restrict_access_allow_coredumps(TRUE);
}

static void main_init(void)
{
	static const struct setting_parser_info *set_roots[] = {
		&quota_status_setting_parser_info,
		NULL
	};
	struct mail_storage_service_input input;
	struct setting_parser_context *set_parser;
	const struct mail_user_settings *user_set;
	const char *value, *error;

	clients = connection_list_init(&client_set, &client_vfuncs);
	storage_service = mail_storage_service_init(master_service, set_roots,
		MAIL_STORAGE_SERVICE_FLAG_ALLOW_ROOT |
		MAIL_STORAGE_SERVICE_FLAG_USERDB_LOOKUP |
		MAIL_STORAGE_SERVICE_FLAG_TEMP_PRIV_DROP |
		MAIL_STORAGE_SERVICE_FLAG_ENABLE_CORE_DUMPS |
		MAIL_STORAGE_SERVICE_FLAG_NO_CHDIR);

	i_zero(&input);
	input.service = "quota-status";
	input.username = "";

	if (mail_storage_service_read_settings(storage_service, &input,
					       &set_parser,
					       &error) < 0)
		i_fatal("%s", error);

	if (master_service_settings_parser_get(NULL, set_parser,
			&mail_user_setting_parser_info,
			MASTER_SERVICE_SETTINGS_GET_FLAG_NO_EXPAND,
			&user_set, &error) < 0)
		i_fatal("%s", error);
	quota_status_settings = master_service_settings_get_or_fatal(NULL,
		&quota_status_setting_parser_info);

	value = mail_user_set_plugin_getenv(user_set, "quota_status_nouser");
	nouser_reply = i_strdup(value != NULL ? value : "REJECT Unknown user");
	master_service_settings_free(user_set);
}

static void main_deinit(void)
{
	master_service_settings_free(quota_status_settings);
	i_free(nouser_reply);
	connection_list_deinit(&clients);
	mail_storage_service_deinit(&storage_service);
}

int main(int argc, char *argv[])
{
	enum master_service_flags service_flags = 0;
	int c;

	protocol = QUOTA_PROTOCOL_UNKNOWN;
	master_service = master_service_init("quota-status", service_flags,
					     &argc, &argv, "p:");
	while ((c = master_getopt(master_service)) > 0) {
		switch (c) {
		case 'p':
			if (strcmp(optarg, "postfix") == 0)
				protocol = QUOTA_PROTOCOL_POSTFIX;
			else
				i_fatal("Unknown -p parameter: '%s'", optarg);
			break;
		default:
			return FATAL_DEFAULT;
		}
	}
	if (protocol == QUOTA_PROTOCOL_UNKNOWN)
		i_fatal("Missing -p parameter");

	master_service_init_log(master_service);
	main_preinit();

	main_init();
	master_service_init_finish(master_service);
	master_service_run(master_service, client_connected);
	main_deinit();
	master_service_deinit(&master_service);
	return 0;
}
