#ifndef __CLIENT_H
#define __CLIENT_H

#include "network.h"
#include "master.h"
#include "client-common.h"
#include "auth-client.h"

struct pop3_client {
	struct client common;

	time_t created;
	int refcount;

	struct io *io;
	struct istream *input;
	struct ostream *output;

	struct login_proxy *proxy;
	char *proxy_user, *proxy_password;
	int proxy_state;

	time_t last_input;
	unsigned int bad_counter;

	char *last_user;

	char *apop_challenge;
	struct auth_connect_id auth_id;

	unsigned int authenticating:1;
	unsigned int auth_connected:1;
	unsigned int destroyed:1;
};

void client_destroy(struct pop3_client *client, const char *reason);
void client_destroy_internal_failure(struct pop3_client *client);

void client_send_line(struct pop3_client *client, const char *line);

bool client_read(struct pop3_client *client);
void client_input(struct pop3_client *client);

void client_ref(struct pop3_client *client);
bool client_unref(struct pop3_client *client);

#endif
