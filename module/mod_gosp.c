/*****************************************
 * Apache module for Go Server Pages     *
 *                                       *
 * By Scott Pakin <scott+gosp@pakin.org> *
 *****************************************/

#include "gosp.h"

/* Forward-declare our module. */
module AP_MODULE_DECLARE_DATA gosp_module;

/* Assign a value to the work directory. */
const char *gosp_set_work_dir(cmd_parms *cmd, void *cfg, const char *arg)
{
  gosp_config_t *config;    /* Server configuration */
  config = ap_get_module_config(cmd->server->module_config, &gosp_module);
  config->work_dir = ap_server_root_relative(cmd->pool, arg);
  return NULL;
}

/* Map a user name to a user ID. */
const char *gosp_set_user_id(cmd_parms *cmd, void *cfg, const char *arg)
{
  gosp_config_t *config;       /* Server configuration */
  apr_uid_t user_id;           /* User ID encountered */
  apr_gid_t group_id;          /* Group ID encountered */
  apr_status_t status;         /* Status of an APR call */

  config = ap_get_module_config(cmd->server->module_config, &gosp_module);
  if (arg[0] == '#') {
    /* Hash followed by a user ID: convert the ID from a string to an
     * integer. */
    config->user_id = (apr_uid_t) apr_atoi64(arg + 1);
  }
  else {
    /* User name: look up the corresponding user ID. */
    status = apr_uid_get(&user_id, &group_id, arg, cmd->temp_pool);
    if (status != APR_SUCCESS)
      return "Failed to map configuration option User to a user ID";
    config->user_id = user_id;
  }
  return NULL;
}

/* Map a group name to a group ID. */
const char *gosp_set_group_id(cmd_parms *cmd, void *cfg, const char *arg)
{
  gosp_config_t *config;       /* Server configuration */
  apr_gid_t group_id;          /* Group ID encountered */
  apr_status_t status;         /* Status of an APR call */

  config = ap_get_module_config(cmd->server->module_config, &gosp_module);
  if (arg[0] == '#') {
    /* Hash followed by a group ID: convert the ID from a string to an
     * integer. */
    config->group_id = (apr_uid_t) apr_atoi64(arg + 1);
  }
  else {
    /* Group name: look up the corresponding group ID. */
    status = apr_gid_get(&group_id, arg, cmd->temp_pool);
    if (status != APR_SUCCESS)
      return "Failed to map configuration option Group to a group ID";
    config->group_id = group_id;
  }
  return NULL;
}

/* Define all of the configuration-file directives we accept. */
static const command_rec gosp_directives[] =
  {
   AP_INIT_TAKE1("GospWorkDir", gosp_set_work_dir, NULL, OR_ALL,
                 "Name of a directory in which Gosp can generate files needed during execution"),
   AP_INIT_TAKE1("User", gosp_set_user_id, NULL, OR_ALL,
                 "The user under which the server will answer requests"),
   AP_INIT_TAKE1("Group", gosp_set_group_id, NULL, OR_ALL,
                 "The group under which the server will answer requests"),
   { NULL }
  };

/* Allocate and initialize a configuration data structure. */
static void *gosp_allocate_server_config(apr_pool_t *p, server_rec *s)
{
  gosp_config_t *config;

  config = apr_palloc(p, sizeof(gosp_config_t));
  config->work_dir = ap_server_root_relative(p, DEFAULT_WORK_DIR);
  config->user_id = 0;
  config->group_id = 0;
  return (void *) config;
}

/* Run after the configuration file has been processed but before lowering
 * privileges. */
static int gosp_post_config(apr_pool_t *pconf, apr_pool_t *plog,
                            apr_pool_t *ptemp, server_rec *s)
{
  gosp_config_t *config;      /* Server configuration */
  apr_status_t status;        /* Status of an APR call */

  /* Create our work directory. */
  config = ap_get_module_config(s->module_config, &gosp_module);
  ap_log_error(APLOG_MARK, APLOG_NOERRNO|APLOG_NOTICE, APR_SUCCESS, s,
               "Using %s as Gosp's work directory", config->work_dir);
  if (create_directories_for(s, ptemp, config->work_dir, 1) != GOSP_STATUS_OK)
    return HTTP_INTERNAL_SERVER_ERROR;

  /* Create a global lock.  Store the mutex structure and name of the
   * underlying file in our configuration structure. */
  config->lock_name = concatenate_filepaths(s, pconf, config->work_dir,
                                            "global.lock", NULL);
  if (config->lock_name == NULL)
    return HTTP_INTERNAL_SERVER_ERROR;
  status = apr_global_mutex_create(&config->mutex, config->lock_name,
                                   APR_LOCK_DEFAULT, pconf);
  if (status != APR_SUCCESS)
    REPORT_ERROR(HTTP_INTERNAL_SERVER_ERROR, APLOG_ALERT, status,
                 "Failed to create lock file %s", config->lock_name);
#ifdef AP_NEED_SET_MUTEX_PERMS
  status = ap_unixd_set_global_mutex_perms(config->mutex);
  if (status != APR_SUCCESS)
    REPORT_ERROR(HTTP_INTERNAL_SERVER_ERROR, APLOG_ALERT, status,
                 "Failed to set permissions on lock file %s", config->lock_name);
#endif
  return OK;
}

/* Perform per-child initialization. */
static void gosp_child_init(apr_pool_t *pool, server_rec *s)
{
  gosp_config_t *config;     /* Server configuration */
  apr_status_t status;       /* Status of an APR call */

  /* Reconnect to the global mutex. */
  config = ap_get_module_config(s->module_config, &gosp_module);
  status = apr_global_mutex_child_init(&config->mutex, config->lock_name, pool);
  if (status != APR_SUCCESS)
    ap_log_error(APLOG_MARK, APLOG_ALERT, status, s,
                 "Failed to reconnect to lock file %s", config->lock_name);
}

/* Handle requests of type "gosp" by passing them to the gosp2go tool. */
static int gosp_handler(request_rec *r)
{
  apr_status_t status;       /* Status of an APR call */
  int launch_status;         /* Status returned by launch_gosp_process() */
  char *sock_name;           /* Name of the Unix-domain socket to connect to */
  apr_socket_t *sock;        /* The Unix-domain socket proper */
  apr_finfo_t finfo;         /* File information for the rquested file */
  gosp_config_t *config;     /* Server configuration */

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

  /* Acquire access to our configuration information. */
  config = ap_get_module_config(r->server->module_config, &gosp_module);

  /* Go Server Pages are always expressed in HTML. */
  r->content_type = "text/html";

  /* Temporary placeholder */
  ap_rprintf(r, "Translating %s\n", r->filename);
  return OK;
}

/* Invoke gosp_handler at the end of every request. */
static void gosp_register_hooks(apr_pool_t *p)
{
  ap_hook_post_config(gosp_post_config, NULL, NULL, APR_HOOK_LAST);
  ap_hook_child_init(gosp_child_init, NULL, NULL, APR_HOOK_LAST);
  ap_hook_handler(gosp_handler, NULL, NULL, APR_HOOK_LAST);
}

/* Dispatch list for API hooks */
module AP_MODULE_DECLARE_DATA gosp_module =
  {
   STANDARD20_MODULE_STUFF,
   NULL,                         /* Allocate per-server configuration */
   NULL,                         /* Merge per-server configurations */
   gosp_allocate_server_config,  /* Allocate per-server configuration */
   NULL,                         /* Merge per-server configurations */
   gosp_directives,              /* Define our configuration directives */
   gosp_register_hooks           /* Register Gosp hooks */
  };
