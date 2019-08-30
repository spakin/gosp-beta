/******************************************
 * Communicate with a Gosp server process *
 *                                        *
 * By Scott Pakin <scott+gosp@pakin.org>  *
 ******************************************/

#include "gosp.h"

/* Send a string to the socket.  Log a message and return GOSP_STATUS_FAIL on
 * error. */
#define SEND_STRING(...)                                                \
  do {                                                                  \
    const char *str = apr_psprintf(r->pool, __VA_ARGS__);               \
    apr_size_t exp_len = (apr_size_t) strlen(str);                      \
    apr_size_t len = exp_len;                                           \
                                                                        \
    status = apr_socket_send(sock, str, &len);                          \
    if (status != APR_SUCCESS || len != exp_len)                        \
      REPORT_REQUEST_ERROR(GOSP_STATUS_FAIL, APLOG_ALERT, status,       \
                          "Failed to send %lu bytes to the Gosp server", (unsigned long)exp_len); \
  }                                                                     \
  while (0)

/* Connect to a Unix-domain stream socket.  Return GOSP_STATUS_FAIL if we fail
 * to create any local data structures.  Return GOSP_STATUS_NEED_ACTION if we
 * fail to connect to the socket.  Return GOSP_STATUS_OK on success. */
gosp_status_t connect_socket(request_rec *r, const char *sock_name, apr_socket_t **sock)
{
  apr_sockaddr_t *sa;         /* Socket address corresponding to sock_name */
  apr_status_t status;        /* Status of an APR call */

  /* Construct a socket address. */
  status = apr_sockaddr_info_get(&sa, sock_name, APR_UNIX, 0, 0, r->pool);
  if (status != APR_SUCCESS)
    REPORT_REQUEST_ERROR(GOSP_STATUS_FAIL, APLOG_ALERT, status,
                         "Failed to construct a Unix-domain socket address from %s", sock_name);

  /* Create a Unix-domain stream socket. */
  status = apr_socket_create(sock, APR_UNIX, SOCK_STREAM, APR_PROTO_TCP, r->pool);
  if (status != APR_SUCCESS)
    REPORT_REQUEST_ERROR(GOSP_STATUS_FAIL, APLOG_ALERT, status,
                         "Failed to create socket %s", sock_name);

  /* Connect to the socket we just created.  Failure presumably indicates that
   * the Gosp server isn't running. */
  status = apr_socket_connect(*sock, sa);
  if (status != APR_SUCCESS)
    REPORT_REQUEST_ERROR(GOSP_STATUS_NEED_ACTION, APLOG_INFO, status,
                         "Failed to connect to socket %s", sock_name);
  return GOSP_STATUS_OK;
}

/* Escape a string for JSON. */
static char *escape_for_json(request_rec *r, const char *str)
{
  char *quoted;    /* Quoted version of str */
  const char *sp;  /* Pointer into str */
  char *qp;        /* Pointer into quoted */

  if (str == NULL)
    return apr_pstrdup(r->pool, "");
  quoted = apr_palloc(r->pool, strlen(str)*2 + 1);  /* Worst-case allocation */
  for (qp = quoted, sp = str; *sp != '\0'; qp++, sp++)
    switch (*sp) {
    case '\\':
    case '"':
      *qp++ = '\\';
      *qp = *sp;
      break;

    default:
      *qp = *sp;
      break;
    }
  *qp = '\0';
  return quoted;
}

/* Send HTTP connection information to a socket.  The connection information
 * must be kept up-to-date with the GospRequest struct in boilerplate.go. */
gosp_status_t send_request(request_rec *r, apr_socket_t *sock)
{
  apr_status_t status;        /* Status of an APR call */
  const char *rhost;          /* Name of remote host */

  rhost = ap_get_remote_host(r->connection, r->per_dir_config, REMOTE_NAME, NULL);
  SEND_STRING("{\n");
  SEND_STRING("  \"LocalHostname\": \"%s\",\n", escape_for_json(r, r->hostname));
  SEND_STRING("  \"QueryArgs\": \"%s\",\n", escape_for_json(r, r->args));
  SEND_STRING("  \"PathInfo\": \"%s\",\n", escape_for_json(r, r->path_info));
  SEND_STRING("  \"Uri\": \"%s\",\n", escape_for_json(r, r->uri));
  SEND_STRING("  \"RemoteHostname\": \"%s\"\n", escape_for_json(r, rhost));
  SEND_STRING("}\n");
  return GOSP_STATUS_OK;
}

/* Ask a Gosp server to shut down cleanly.  The termination command must be
 * kept up-to-date with the GospRequest struct in boilerplate.go. */
gosp_status_t send_termination_request(request_rec *r, const char *sock_name)
{
  char *response;             /* Response string */
  size_t resp_len;            /* Length of response string */
  apr_proc_t proc;            /* Gosp server process */
  apr_time_t begin_time;      /* Time at which we began waiting for the server to terminate */
  apr_socket_t *sock;         /* Socket with which to communicate with the Gosp server */
  gosp_status_t gstatus;      /* Status of an internal Gosp call */
  apr_status_t status;        /* Status of an APR call */

  /* Connect to the process that handles the requested Go Server Page. */
  ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_INFO, APR_SUCCESS, r,
               "Asking the Gosp server listening on socket %s to terminate",
               sock_name);
  gstatus = connect_socket(r, sock_name, &sock);
  if (gstatus != GOSP_STATUS_OK)
    return GOSP_STATUS_NEED_ACTION;

  /* Ask the server to terminate. */
  SEND_STRING("{\n");
  SEND_STRING("  \"ExitNow\": \"true\"\n");
  SEND_STRING("}\n");

  /* Receive a process ID in response. */
  gstatus = receive_response(r, sock, &response, &resp_len);
  if (gstatus != GOSP_STATUS_OK)
    return GOSP_STATUS_FAIL;
  if (strncmp(response, "gosp-pid ", 9) != 0)
    return GOSP_STATUS_FAIL;
  proc.pid = atoi(response + 9);
  if (proc.pid <= 0)
    return GOSP_STATUS_FAIL;

  /* We no longer need the socket. */
  status = apr_socket_close(sock);
  if (status != APR_SUCCESS)
    return GOSP_STATUS_FAIL;

  /* Wait for a short time for the process to exit by itself. */
  begin_time = apr_time_now();
  while (apr_time_now() - begin_time < GOSP_EXIT_WAIT_TIME) {
    /* Ping the process.  If it's not found, it must have exited on its own. */
    status = apr_proc_kill(&proc, 0);
    if (APR_TO_OS_ERROR(status) == ESRCH)
      return GOSP_STATUS_OK;
    apr_sleep(1000);
  }

  /* The process did not exist by itself.  Kill it. */
  status = apr_proc_kill(&proc, SIGKILL);
  if (status != APR_SUCCESS)
    return GOSP_STATUS_FAIL;
  return GOSP_STATUS_OK;
}

/* Split a response string into metadata and data.  Process the metadata.
 * Output the data.  Return GOSP_STATUS_OK if this procedure succeeded (even if
 * it corresponds to a Gosp-server error condition) or GOSP_STATUS_FAIL if
 * not. */
static gosp_status_t process_response(request_rec *r, char *response, size_t resp_len)
{
  char *last;      /* Internal apr_strtok() state */
  char *line;      /* One line of metadata */
  int n_to_write;  /* Number of bytes of data we expect to write */
  int nwritten;    /* Number of bytes of data actuall written */

  /* Process each line of metadata until we see "end-header". */
  for (line = apr_strtok(response, "\n", &last);
       line != NULL;
       line = apr_strtok(NULL, "\n", &last)) {
    /* End of metadata: exit loop. */
    if (strcmp(line, "end-header") == 0)
      break;

    /* Heartbeat: ignore. */
    if (strcmp(line, "keep-alive") == 0)
      continue;

    /* HTTP status: set in the request_rec. */
    if (strncmp(line, "http-status ", 12) == 0) {
      r->status = atoi(line + 12);
      if (r->status < 100)
        return GOSP_STATUS_FAIL;
      continue;
    }

    /* MIME type: set in the request_rec. */
    if (strncmp(line, "mime-type ", 10) == 0) {
      r->content_type = line + 10;
      continue;
    }

    /* Anything else: throw an error. */
    return GOSP_STATUS_FAIL;
  }

  /* Write the rest of the response as data. */
  if (r->status != HTTP_OK)
    return GOSP_STATUS_OK;
  if (line == NULL)
    return GOSP_STATUS_OK;
  n_to_write = (int)resp_len - (int)(last - response + 1) - 11;  /* 11 is "end-header" followed by apr_strtok's '\0'. */
  nwritten = ap_rwrite(line + 11, n_to_write, r);
  if (nwritten != n_to_write)
    return GOSP_STATUS_FAIL;
  return GOSP_STATUS_OK;
}

/* Receive a response from the Gosp server and return it.  Return
 * GOSP_STATUS_NEED_ACTION if the server timed out and ought to be killed and
 * relaunched. */
gosp_status_t receive_response(request_rec *r, apr_socket_t *sock, char **response, size_t *resp_len)
{
  char *chunk;                /* One chunk of data read from the socket */
  const size_t chunk_size = 1000000;   /* Amount of data to read at once */
  struct iovec iov[2];        /* Pairs of chunks to merge */
  apr_status_t status;        /* Status of an APR call */

  /* Prepare to read from the socket. */
  status = apr_socket_timeout_set(sock, GOSP_RESPONSE_TIMEOUT);
  if (status != APR_SUCCESS)
    REPORT_REQUEST_ERROR(GOSP_STATUS_FAIL, APLOG_ALERT, status,
                         "Failed to set a socket timeout");
  chunk = apr_palloc(r->pool, chunk_size);
  iov[0].iov_base = "";
  iov[0].iov_len = 0;
  iov[1].iov_base = chunk;

  /* Read until the socket is closed. */
  while (status != APR_EOF) {
    apr_size_t len = chunk_size;  /* Number of bytes to read/just read */

    /* Read one chunk of data. */
    status = apr_socket_recv(sock, chunk, &len);
    switch (status) {
    case APR_EOF:
    case APR_SUCCESS:
      /* Successful read */
      break;

    case APR_TIMEUP:
      /* Timeout occurred */
      return GOSP_STATUS_NEED_ACTION;
      break;

    default:
      /* Other error */
      REPORT_REQUEST_ERROR(GOSP_STATUS_FAIL, APLOG_ALERT, status,
                           "Failed to receive data from the Gosp server");
      break;
    }

    /* Append the new chunk onto the aggregate response. */
    iov[1].iov_len = (size_t) len;
    iov[0].iov_base = apr_pstrcatv(r->pool, iov, 2, &len);
    iov[0].iov_len = (size_t) len;
  }

  /* Return the string and its length. */
  *response = iov[0].iov_base;
  *resp_len = iov[0].iov_len;
  return GOSP_STATUS_OK;
}

/* Send a request to the Gosp server and process its response.  If the server
 * is not currently running, return GOSP_STATUS_NEED_ACTION.  This function is
 * intended to represent the common case in processing HTTP requests to Gosp
 * pages. */
gosp_status_t simple_request_response(request_rec *r, const char *sock_name)
{
  apr_socket_t *sock;         /* The Unix-domain socket proper */
  char *response;             /* Response string */
  size_t resp_len;            /* Length of response string */
  gosp_config_t *config;      /* Server configuration */
  apr_status_t status;        /* Status of an APR call */
  gosp_status_t gstatus;      /* Status of an internal Gosp call */

  /* Acquire access to our configuration information. */
  config = ap_get_module_config(r->server->module_config, &gosp_module);

  /* Connect to the process that handles the requested Go Server Page. */
  ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_DEBUG, APR_SUCCESS, r,
               "Asking the Gosp server listening on socket %s to handle URI %s",
               sock_name, r->uri);
  gstatus = connect_socket(r, sock_name, &sock);
  if (gstatus != GOSP_STATUS_OK)
    return gstatus;

  /* Send the Gosp server a request and process its response. */
  gstatus = send_request(r, sock);
  if (gstatus != GOSP_STATUS_OK)
    return GOSP_STATUS_FAIL;
  gstatus = receive_response(r, sock, &response, &resp_len);
  if (gstatus != GOSP_STATUS_OK)
    return GOSP_STATUS_FAIL;
  status = apr_socket_close(sock);
  if (status != APR_SUCCESS)
    return GOSP_STATUS_FAIL;
  return process_response(r, response, resp_len);
}
