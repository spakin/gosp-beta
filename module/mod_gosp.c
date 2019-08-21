/*****************************************
 * Apache module for Go Server Pages     *
 *                                       *
 * By Scott Pakin <scott+gosp@pakin.org> *
 *****************************************/

#include "gosp.h"

/* Define our configuration options. */
static config_t config;

/* Assign a value to the work directory. */
const char *gosp_set_work_dir(cmd_parms *cmd, void *cfg, const char *arg)
{
  config.work_dir = arg;
  return NULL;
}

/* Map a user name to a user ID. */
const char *gosp_set_user_id(cmd_parms *cmd, void *cfg, const char *arg)
{
  apr_uid_t user_id;         /* User ID encountered */
  apr_gid_t group_id;        /* Group ID encountered */
  apr_status_t status;       /* Status of an APR call */

  if (arg[0] == '#') {
    /* Hash followed by a user ID: convert the ID from a string to an
     * integer. */
    config.user_id = (apr_uid_t) apr_atoi64(arg + 1);
  }
  else {
    /* User name: look up the corresponding user ID. */
    status = apr_uid_get(&user_id, &group_id, arg, cmd->temp_pool);
    if (status != APR_SUCCESS)
      return "Failed to map configuration option User to a user ID";
    config.user_id = user_id;
  }
  return NULL;
}

/* Map a group name to a group ID. */
const char *gosp_set_group_id(cmd_parms *cmd, void *cfg, const char *arg)
{
  apr_gid_t group_id;        /* Group ID encountered */
  apr_status_t status;       /* Status of an APR call */

  if (arg[0] == '#') {
    /* Hash followed by a group ID: convert the ID from a string to an
     * integer. */
    config.group_id = (apr_uid_t) apr_atoi64(arg + 1);
  }
  else {
    /* Group name: look up the corresponding group ID. */
    status = apr_gid_get(&group_id, arg, cmd->temp_pool);
    if (status != APR_SUCCESS)
      return "Failed to map configuration option Group to a group ID";
    config.group_id = group_id;
  }
  return NULL;
}

/* Define all of the configuration-file directives we accept. */
static const command_rec gosp_directives[] =
  {
   AP_INIT_TAKE1("GospWorkDir", gosp_set_work_dir, NULL, RSRC_CONF|ACCESS_CONF,
                 "Name of a directory in which Gosp can generate files needed during execution"),
   AP_INIT_TAKE1("User", gosp_set_user_id, NULL, RSRC_CONF|ACCESS_CONF,
                 "The user under which the server will answer requests"),
   AP_INIT_TAKE1("Group", gosp_set_group_id, NULL, RSRC_CONF|ACCESS_CONF,
                 "The group under which the server will answer requests"),
   { NULL }
  };

/* Handle requests of type "gosp" by passing them to the gosp2go tool. */
static int gosp_handler(request_rec *r)
{
  apr_status_t status;       /* Status of an APR call */
  int launch_status;         /* Status returned by launch_gosp_process() */
  char *sock_name;           /* Name of the Unix-domain socket to connect to */
  apr_socket_t *sock;        /* The Unix-domain socket proper */
  apr_finfo_t finfo;         /* File information for the rquested file */
  char *lock_name;           /* Name of a lock file associated with the request */
  apr_global_mutex_t *lock;  /* Lock associated with the request */

  /* We care only about "gosp" requests, and we don't care about HEAD
   * requests. */
  if (strcmp(r->handler, "gosp"))
    return DECLINED;
  if (r->header_only)
    return DECLINED;

  /* Issue an HTTP error if the requested Gosp file doesn't exist. */
  status = apr_stat(&finfo, r->canonical_filename, 0, r->pool);
  if (status != APR_SUCCESS)
    return HTTP_NOT_FOUND;

  /* Create and prepare the cache work directory.  Although it would be nice to
   * hoist this into the post-config handler, that runs before switching users
   * while gosp_handler runs after switching users.  Creating a directory in a
   * post-config handler would therefore lead to permission-denied errors. */
  if (prepare_config_directory(r, "work", &config.work_dir, DEFAULT_WORK_DIR, "GospWorkDir") != GOSP_STATUS_OK)
    return HTTP_INTERNAL_SERVER_ERROR;

  /* Associate a lock file with the Go Server Page. */
  lock_name = concatenate_filepaths(r, config.work_dir, "locks", r->canonical_filename, NULL);
  if (lock_name == NULL)
    return HTTP_INTERNAL_SERVER_ERROR;
  lock_name = apr_pstrcat(r->pool, lock_name, ".lock", NULL);
  ap_log_error(APLOG_MARK, APLOG_NOERRNO|APLOG_DEBUG, APR_SUCCESS, r->server,
               "Using %s as the lock file", lock_name);
  if (create_directories_for(r, lock_name) != GOSP_STATUS_OK)
    return HTTP_INTERNAL_SERVER_ERROR;
  status = apr_global_mutex_create(&lock, lock_name, APR_LOCK_DEFAULT, r->pool);
  if (status != APR_SUCCESS)
    REPORT_ERROR(HTTP_INTERNAL_SERVER_ERROR, APLOG_ALERT, status,
                 "Failed to create lock file %s", lock_name);

  /* Connect to the process that handles the requested Go Server Page. */
  sock_name = concatenate_filepaths(r, config.work_dir, "sockets", r->canonical_filename, NULL);
  if (sock_name == NULL)
    return HTTP_INTERNAL_SERVER_ERROR;
  sock_name = apr_pstrcat(r->pool, sock_name, ".sock", NULL);
  ap_log_error(APLOG_MARK, APLOG_NOERRNO|APLOG_DEBUG, APR_SUCCESS, r->server,
               "Communicating with a Gosp server via socket %s", sock_name);
  status = connect_socket(&sock, r, sock_name);

  /* Temporary */
  /*
  ap_log_error(APLOG_MARK, APLOG_NOTICE, status, r->server,
               "Connecting to socket %s returned %d", sock_name, status);
  */
  launch_status = compile_gosp_server(r, config.work_dir);
  if (launch_status != GOSP_STATUS_OK)
    ap_log_error(APLOG_MARK, APLOG_NOTICE, APR_SUCCESS, r->server,
                 "Failed to compile %s (code %d)", r->canonical_filename, launch_status);
  /*
  launch_status = launch_gosp_process(r, config.work_dir, sock_name);
  if (launch_status != GOSP_STATUS_OK)
    ap_log_error(APLOG_MARK, APLOG_NOTICE, APR_SUCCESS, r->server,
                 "Failed to launch %s (code %d)", r->canonical_filename, launch_status);
  */

  /* Go Server Pages are always expressed in HTML. */
  r->content_type = "text/html";

  /* Temporary placeholder */
  ap_rprintf(r, "Translating %s\n", r->filename);
  return OK;
}

/* Invoke gosp_handler at the end of every request. */
static void gosp_register_hooks(apr_pool_t *p)
{
  ap_hook_handler(gosp_handler, NULL, NULL, APR_HOOK_LAST);
}

/* Dispatch list for API hooks */
module AP_MODULE_DECLARE_DATA gosp_module =
  {
   STANDARD20_MODULE_STUFF,
   NULL,                /* Per-directory configuration handler */
   NULL,                /* Merge handler for per-directory configurations */
   NULL,                /* Per-server configuration handler */
   NULL,                /* Merge handler for per-server configurations */
   gosp_directives,     /* Any directives we may have for httpd */
   gosp_register_hooks  /* Register Gosp hooks */
  };
