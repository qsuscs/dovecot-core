/* Copyright (C) 2006 Timo Sirainen */

#include "lib.h"
#include "hash.h"
#include "acl-cache.h"
#include "acl-api-private.h"

#include <stdlib.h>

extern struct acl_backend_vfuncs acl_backend_vfile;

static const char *const owner_mailbox_rights[] = {
	MAIL_ACL_LOOKUP,
	MAIL_ACL_READ,
	MAIL_ACL_WRITE,
	MAIL_ACL_WRITE_SEEN,
	MAIL_ACL_WRITE_DELETED,
	MAIL_ACL_INSERT,
	MAIL_ACL_EXPUNGE,
	MAIL_ACL_CREATE,
	MAIL_ACL_DELETE,
	MAIL_ACL_ADMIN,
	NULL
};

static const char *const non_owner_mailbox_rights[] = { NULL };

struct acl_backend *
acl_backend_init(const char *data, struct mail_storage *storage,
		 const char *acl_username, const char *const *groups,
		 const char *owner_username)
{
	struct acl_backend *backend;
	unsigned int i, group_count;
	bool storage_owner, debug;

	debug = getenv("DEBUG") != NULL;
	if (debug) {
		i_info("acl: initializing backend with data: %s", data);
		i_info("acl: acl username = %s", acl_username);
		i_info("acl: owner username = %s",
		       owner_username != NULL ? owner_username : "");
	}

	group_count = strarray_length(groups);

	if (strncmp(data, "vfile:", 6) == 0)
		data += 6;
	else if (strcmp(data, "vfile") == 0)
		data = "";
	else
		i_fatal("Unknown ACL backend: %s", t_strcut(data, ':'));

	backend = acl_backend_vfile.alloc();
	backend->debug = debug;
	backend->v = acl_backend_vfile;
	backend->storage = storage;
	backend->username = p_strdup(backend->pool, acl_username);
	backend->owner_username = owner_username == NULL ? "" :
		p_strdup(backend->pool, owner_username);

	if (group_count > 0) {
		backend->group_count = group_count;
		backend->groups =
			p_new(backend->pool, const char *, group_count);
		for (i = 0; i < group_count; i++)
			backend->groups[i] = groups[i];
		qsort(backend->groups, group_count, sizeof(const char *),
		      strcmp_p);
	}

	if (acl_backend_vfile.init(backend, data) < 0)
		i_fatal("acl: backend vfile init failed with data: %s", data);

	storage_owner = owner_username != NULL &&
		strcmp(acl_username, owner_username) == 0;
	backend->default_aclmask =
		acl_cache_mask_init(backend->cache, backend->pool,
				    storage_owner ? owner_mailbox_rights :
				    non_owner_mailbox_rights);

	backend->default_aclobj = acl_object_init_from_name(backend, "");
	return backend;
}

void acl_backend_deinit(struct acl_backend **_backend)
{
	struct acl_backend *backend = *_backend;

	*_backend = NULL;

	acl_object_deinit(&backend->default_aclobj);
	acl_cache_deinit(&backend->cache);
	backend->v.deinit(backend);
}

bool acl_backend_user_is_authenticated(struct acl_backend *backend)
{
	return backend->username != NULL;
}

bool acl_backend_user_name_equals(struct acl_backend *backend,
				  const char *username)
{
	if (backend->username == NULL) {
		/* anonymous user never matches */
		return FALSE;
	}

	return strcmp(backend->username, username) == 0;
}

bool acl_backend_user_is_in_group(struct acl_backend *backend,
				  const char *group_name)
{
	return bsearch(group_name, backend->groups, backend->group_count,
		       sizeof(const char *), bsearch_strcmp) != NULL;
}

unsigned int acl_backend_lookup_right(struct acl_backend *backend,
				      const char *right)
{
	return acl_cache_right_lookup(backend->cache, right);
}
