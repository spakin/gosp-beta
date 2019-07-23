/*****************************************
 * Apache module for Go Server Pages     *
 *                                       *
 * By Scott Pakin <scott+gosp@pakin.org> *
 *****************************************/

#include "gosp.h"

/* Define our configuration options. */
static config_t config;

/* Assign a value to the cache directory. */
const char *gosp_set_cache_dir(cmd_parms *cmd, void *cfg, const char *arg)
{
  config.cache_dir = arg;
  return NULL;
}

/* Assign a value to the run directory. */
const char *gosp_set_run_dir(cmd_parms *cmd, void *cfg, const char *arg)
{
  config.run_dir = arg;
  return NULL;
}

/* Define all of the configuration-file directives we accept. */
static const command_rec gosp_directives[] =
  {
   AP_INIT_TAKE1("GospCacheDir", gosp_set_cache_dir, NULL, RSRC_CONF,
                 "Name of a directory in which to cache generated files"),
   AP_INIT_TAKE1("GospRunDir", gosp_set_run_dir, NULL, RSRC_CONF,
                 "Name of a directory in which to keep files needed only during server execution"),
   { NULL }
  };

/* Handle requests of type "gosp" by passing them to the gosp2go tool. */
static int gosp_handler(request_rec *r)
{
  apr_status_t status;    /* Status of an APR call */
  int launch_status;      /* Status returned by launch_gosp_process() */
  char *sock_name;        /* Name of the Unix-domain socket to connect to */
  apr_socket_t *sock;     /* The Unix-domain socket proper */

  /* We care only about "gosp" requests, and we don't care about HEAD
   * requests. */
  if (strcmp(r->handler, "gosp"))
    return DECLINED;
  if (r->header_only)
    return DECLINED;

  /* Create and prepare the cache and run directories.  Although it would be
   * nice to hoist this into the post-config handler, that runs before
   * switching users while gosp_handler runs after switching users.  Creating
   * directories in a post-config handler would therefore lead to
   * permission-denied errors. */
  if (!prepare_config_directory(r, "cache", &config.cache_dir, DEFAULT_CACHE_DIR, "GospCacheDir"))
    return HTTP_INTERNAL_SERVER_ERROR;
  if (!prepare_config_directory(r, "run", &config.run_dir, DEFAULT_RUN_DIR, "GospRunDir"))
    return HTTP_INTERNAL_SERVER_ERROR;

  /* Connect to the process that handles the requested Go Server Page. */
  sock_name = append_filepaths(r, config.run_dir, r->canonical_filename);
  if (sock_name == NULL)
    return HTTP_INTERNAL_SERVER_ERROR;
  sock_name = apr_pstrcat(r->pool, sock_name, ".sock", NULL);
  ap_log_error(APLOG_MARK, APLOG_NOERRNO|APLOG_DEBUG, APR_SUCCESS, r->server,
               "Communicating with a Gosp server via socket %s", sock_name);
  status = connect_socket(&sock, r, sock_name);

  /* Temporary */
  ap_log_error(APLOG_MARK, APLOG_NOTICE, status, r->server,
               "Connecting to socket %s returned %d", sock_name, status);
  launch_status = launch_gosp_process(r, config.run_dir, sock_name);
  if (launch_status != GOSP_LAUNCH_OK)
    ap_log_error(APLOG_MARK, APLOG_NOTICE, APR_SUCCESS, r->server,
		 "Failed to launch %s (code %d)", r->canonical_filename, launch_status);

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
