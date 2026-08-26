#include <nuttx/config.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <netutils/netlib.h>
#ifdef CONFIG_NETUTILS_NTPCLIENT
#  include <netutils/ntpclient.h>
#endif
#include "quickjs.h"
#include "qpk_net.h"

#define QPK_HTTP_MAX_BODY  (128 * 1024)
#define QPK_HTTP_MAX_REQUESTS 2
#define QPK_HTTP_WORKER_STACK (24 * 1024)
#define QPK_TLS_MIN_TIME    1704067200

extern const unsigned char g_qpk_ca_bundle[];
extern const unsigned char g_qpk_ca_bundle_end[];

struct qpk_tls_s
{
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context ctr_drbg;
  mbedtls_ssl_context ssl;
  mbedtls_ssl_config config;
  mbedtls_net_context net;
  bool established;
};

struct qpk_transport_s
{
  int fd;
  bool tls;
  struct qpk_tls_s *tls_ctx;
};

struct qpk_request_s
{
  struct qpk_request_s *next;
  JSContext *ctx;
  JSValue resolve;
  JSValue reject;
  char *url;
  char *method;
  char *body;
  char *response;
  int timeout;
  int status;
  int result;
  int fd;
  bool done;
  bool canceled;
};

static pthread_mutex_t g_qpk_request_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_qpk_ca_lock = PTHREAD_MUTEX_INITIALIZER;
static struct qpk_request_s *g_qpk_requests;

static uint64_t qpk_monotonic_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int qpk_ssl_send(void *ctx, const unsigned char *buf, size_t len)
{
  mbedtls_net_context *net = ctx;
  ssize_t ret = send(net->fd, buf, len, 0);

  if (ret >= 0)
    {
      return (int)ret;
    }

  if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
    {
      return MBEDTLS_ERR_SSL_WANT_WRITE;
    }

  return -errno;
}

static int qpk_ssl_recv(void *ctx, unsigned char *buf, size_t len)
{
  mbedtls_net_context *net = ctx;
  ssize_t ret = recv(net->fd, buf, len, 0);

  if (ret >= 0)
    {
      return (int)ret;
    }

  if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
    {
      return MBEDTLS_ERR_SSL_WANT_READ;
    }

  if (errno == EPIPE || errno == ECONNRESET)
    {
      return MBEDTLS_ERR_NET_CONN_RESET;
    }

  return -errno;
}

static mbedtls_x509_crt g_qpk_ca;
static bool g_qpk_ca_ready;

static JSValue qpk_network_status(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
  struct in_addr addr;
  char ip[INET_ADDRSTRLEN] = "0.0.0.0";
  JSValue out = JS_NewObject(ctx);
  int connected = netlib_get_ipv4addr("eth0", &addr) == 0 &&
                  addr.s_addr != 0;

  (void)this_val;
  (void)argc;
  (void)argv;
  if (connected)
    {
      inet_ntop(AF_INET, &addr, ip, sizeof(ip));
    }

  JS_SetPropertyStr(ctx, out, "type", JS_NewString(ctx, "wifi"));
  JS_SetPropertyStr(ctx, out, "connected", JS_NewBool(ctx, connected));
  JS_SetPropertyStr(ctx, out, "ip", JS_NewString(ctx, ip));
  JS_SetPropertyStr(ctx, out, "interface", JS_NewString(ctx, "eth0"));
  return out;
}

static JSValue qpk_network_get_type(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
  (void)this_val;
  (void)argc;
  (void)argv;
  return JS_NewString(ctx, "wifi");
}

static int qpk_wait_for_valid_time(int timeout_ms)
{
#ifdef CONFIG_NETUTILS_NTPCLIENT
  int waited = 0;

  if (time(NULL) >= QPK_TLS_MIN_TIME)
    {
      return 0;
    }

  (void)ntpc_start();
  while (time(NULL) < QPK_TLS_MIN_TIME && waited < timeout_ms)
    {
      usleep(100000);
      waited += 100;
    }

  return time(NULL) >= QPK_TLS_MIN_TIME ? 0 : -ETIMEDOUT;
#else
  return time(NULL) >= QPK_TLS_MIN_TIME ? 0 : -ENODATA;
#endif
}

static int qpk_ca_init(void)
{
  int lockret;
  int ret;

  lockret = pthread_mutex_lock(&g_qpk_ca_lock);
  if (lockret != 0)
    {
      return -lockret;
    }

  if (g_qpk_ca_ready)
    {
      ret = 0;
      goto out;
    }

  mbedtls_x509_crt_init(&g_qpk_ca);
  ret = mbedtls_x509_crt_parse(&g_qpk_ca, g_qpk_ca_bundle,
                               g_qpk_ca_bundle_end - g_qpk_ca_bundle);
  if (ret != 0)
    {
      mbedtls_x509_crt_free(&g_qpk_ca);
      ret = ret < 0 ? ret : -EBADMSG;
      goto out;
    }

  g_qpk_ca_ready = true;
  ret = 0;

out:
  pthread_mutex_unlock(&g_qpk_ca_lock);
  return ret;
}

static int qpk_connect(const char *host, const char *port, int timeout_ms)
{
  struct addrinfo hints;
  struct addrinfo *res = NULL;
  struct timeval tv;
  int fd = -1;

  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(host, port, &hints, &res) != 0 || res == NULL)
    {
      return -EHOSTUNREACH;
    }

  fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (fd < 0)
    {
      freeaddrinfo(res);
      return -errno;
    }

  tv.tv_sec = timeout_ms > 0 ? timeout_ms / 1000 : 15;
  tv.tv_usec = (timeout_ms > 0 ? timeout_ms % 1000 : 0) * 1000;
  (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  if (connect(fd, res->ai_addr, res->ai_addrlen) < 0)
    {
      int errcode = errno;
      close(fd);
      freeaddrinfo(res);
      return -errcode;
    }

  freeaddrinfo(res);
  return fd;
}

static void qpk_transport_close(struct qpk_transport_s *transport)
{
  if (transport->tls_ctx != NULL)
    {
      if (transport->tls_ctx->established)
        {
          /* App shutdown must not wait on a peer that has already stopped
           * responding.  Closing the TCP socket is sufficient for a client
           * teardown; close_notify is best effort only. */
          (void)shutdown(transport->fd, SHUT_RDWR);
        }
      mbedtls_ssl_free(&transport->tls_ctx->ssl);
      mbedtls_ssl_config_free(&transport->tls_ctx->config);
      mbedtls_ctr_drbg_free(&transport->tls_ctx->ctr_drbg);
      mbedtls_entropy_free(&transport->tls_ctx->entropy);
      free(transport->tls_ctx);
      transport->tls_ctx = NULL;
    }

  if (transport->fd >= 0)
    {
      close(transport->fd);
      transport->fd = -1;
    }
}

static int qpk_tls_open(struct qpk_transport_s *transport,
                        const char *host, int timeout_ms)
{
  static const unsigned char personal[] = "qpk.fetch";
  struct qpk_tls_s *tls;
  uint64_t deadline;
  int ret;

  printf("[qpk] HTTPS: TCP connected, starting TLS for %s\n", host);

  tls = calloc(1, sizeof(*tls));
  if (tls == NULL)
    {
      return -ENOMEM;
    }

  mbedtls_entropy_init(&tls->entropy);
  mbedtls_ctr_drbg_init(&tls->ctr_drbg);
  mbedtls_ssl_init(&tls->ssl);
  mbedtls_ssl_config_init(&tls->config);
  mbedtls_net_init(&tls->net);
  tls->net.fd = transport->fd;

  ret = mbedtls_ctr_drbg_seed(&tls->ctr_drbg, mbedtls_entropy_func,
                              &tls->entropy, personal,
                              sizeof(personal) - 1);
  if (ret == 0)
    {
      ret = mbedtls_ssl_config_defaults(&tls->config,
                                        MBEDTLS_SSL_IS_CLIENT,
                                        MBEDTLS_SSL_TRANSPORT_STREAM,
                                        MBEDTLS_SSL_PRESET_DEFAULT);
    }

  if (ret == 0)
    {
      mbedtls_ssl_conf_min_tls_version(&tls->config,
                                       MBEDTLS_SSL_VERSION_TLS1_2);
      mbedtls_ssl_conf_max_tls_version(&tls->config,
                                       MBEDTLS_SSL_VERSION_TLS1_2);
      mbedtls_ssl_conf_authmode(&tls->config, MBEDTLS_SSL_VERIFY_REQUIRED);
      mbedtls_ssl_conf_ca_chain(&tls->config, &g_qpk_ca, NULL);
      mbedtls_ssl_conf_rng(&tls->config, mbedtls_ctr_drbg_random,
                           &tls->ctr_drbg);
      mbedtls_ssl_conf_read_timeout(&tls->config,
                                    timeout_ms > 0 ? timeout_ms : 15000);
      ret = mbedtls_ssl_setup(&tls->ssl, &tls->config);
    }

  if (ret == 0)
    {
      ret = mbedtls_ssl_set_hostname(&tls->ssl, host);
    }

  if (ret == 0)
    {
      /* Certificate parsing and entropy setup can be relatively expensive on
       * the first request.  The handshake timeout must cover network I/O,
       * rather than TLS context preparation performed before ClientHello. */
      deadline = qpk_monotonic_ms() +
                 (timeout_ms > 0 ? timeout_ms : 15000);
      mbedtls_ssl_set_bio(&tls->ssl, &tls->net, qpk_ssl_send,
                          qpk_ssl_recv, NULL);
      do
        {
          ret = mbedtls_ssl_handshake(&tls->ssl);
          if (ret == MBEDTLS_ERR_SSL_WANT_READ ||
              ret == MBEDTLS_ERR_SSL_WANT_WRITE)
            {
              /* The blocking BIO waits in recv/send using SO_RCVTIMEO /
               * SO_SNDTIMEO.  Retry WANT_* until the single handshake
               * deadline expires; no select/poll layer is involved. */
              if (qpk_monotonic_ms() >= deadline)
                {
                  ret = MBEDTLS_ERR_SSL_TIMEOUT;
                  break;
                }
            }
        }
      while (ret == MBEDTLS_ERR_SSL_WANT_READ ||
             ret == MBEDTLS_ERR_SSL_WANT_WRITE);
      printf("[qpk] HTTPS: TLS handshake result=%d\n", ret);
    }

  if (ret == 0 && mbedtls_ssl_get_verify_result(&tls->ssl) != 0)
    {
      ret = MBEDTLS_ERR_X509_CERT_VERIFY_FAILED;
    }

  if (ret != 0)
    {
      mbedtls_ssl_free(&tls->ssl);
      mbedtls_ssl_config_free(&tls->config);
      mbedtls_ctr_drbg_free(&tls->ctr_drbg);
      mbedtls_entropy_free(&tls->entropy);
      free(tls);
      return ret;
    }

  transport->tls = true;
  tls->established = true;
  transport->tls_ctx = tls;
  return 0;
}

static ssize_t qpk_transport_write(struct qpk_transport_s *transport,
                                   const void *buf, size_t len)
{
  uint64_t deadline = qpk_monotonic_ms() + 15000;

  if (!transport->tls)
    {
      ssize_t ret = send(transport->fd, buf, len, 0);
      return ret < 0 ? -errno : ret;
    }

  for (; ; )
    {
      int ret = mbedtls_ssl_write(&transport->tls_ctx->ssl, buf, len);
      if (ret == MBEDTLS_ERR_SSL_WANT_READ ||
          ret == MBEDTLS_ERR_SSL_WANT_WRITE)
        {
          if (qpk_monotonic_ms() >= deadline)
            {
              return -ETIMEDOUT;
            }

          continue;
        }

      if (ret >= 0)
        {
          return ret;
        }

      return ret;
    }
}

static int qpk_transport_write_all(struct qpk_transport_s *transport,
                                   const void *buf, size_t len)
{
  const unsigned char *p = buf;

  while (len > 0)
    {
      ssize_t ret = qpk_transport_write(transport, p, len);
      if (ret <= 0)
        {
          return ret < 0 ? (int)ret : -EPIPE;
        }

      p += ret;
      len -= ret;
    }

  return 0;
}

static ssize_t qpk_transport_read(struct qpk_transport_s *transport,
                                  void *buf, size_t len)
{
  uint64_t deadline = qpk_monotonic_ms() + 15000;

  if (!transport->tls)
    {
      ssize_t ret = recv(transport->fd, buf, len, 0);
      return ret < 0 ? -errno : ret;
    }

  for (; ; )
    {
      int ret = mbedtls_ssl_read(&transport->tls_ctx->ssl, buf, len);
      if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
        {
          return 0;
        }

      if (ret != MBEDTLS_ERR_SSL_WANT_READ &&
          ret != MBEDTLS_ERR_SSL_WANT_WRITE)
        {
          return ret;
        }

      if (qpk_monotonic_ms() >= deadline)
        {
          return -ETIMEDOUT;
        }
    }
}

static int qpk_http_request(struct qpk_request_s *owner,
                            const char *url, const char *method,
                            const char *body, int timeout_ms,
                            int *status, char **response)
{
  struct qpk_transport_s transport = { .fd = -1 };
  const char *host_start;
  const char *authority_end;
  const char *path;
  const char *p;
  char host[128];
  char port[8];
  char request[1024];
  char *buf = NULL;
  bool tls;
  size_t host_len;
  size_t body_len = body ? strlen(body) : 0;
  size_t used = 0;
  int ret;
  int request_len;

  if (strncmp(url, "https://", 8) == 0)
    {
      tls = true;
      host_start = url + 8;
      strlcpy(port, "443", sizeof(port));
    }
  else if (strncmp(url, "http://", 7) == 0)
    {
      tls = false;
      host_start = url + 7;
      strlcpy(port, "80", sizeof(port));
    }
  else
    {
      return -EPROTONOSUPPORT;
    }

  path = strchr(host_start, '/');
  authority_end = path != NULL ? path : host_start + strlen(host_start);
  host_len = (size_t)(authority_end - host_start);
  if (host_len == 0 || host_len >= sizeof(host))
    {
      return -EINVAL;
    }

  memcpy(host, host_start, host_len);
  host[host_len] = '\0';
  p = strchr(host, ':');
  if (p != NULL)
    {
      size_t port_len = strlen(p + 1);
      if (port_len == 0 || port_len >= sizeof(port))
        {
          return -EINVAL;
        }

      memcpy(port, p + 1, port_len + 1);
      host[p - host] = '\0';
    }

  if (host[0] == '\0')
    {
      return -EINVAL;
    }

  /* Certificate validation may need NTP on the first request.  Complete
   * time synchronization and one-time CA parsing before opening TCP so the
   * TLS peer is never left idle while the client is still preparing. */

  if (tls)
    {
      ret = qpk_wait_for_valid_time(timeout_ms > 0 ? timeout_ms : 15000);
      if (ret < 0)
        {
          return ret;
        }

      ret = qpk_ca_init();
      if (ret != 0)
        {
          return ret;
        }
    }

  transport.fd = qpk_connect(host, port, timeout_ms);
  if (transport.fd < 0)
    {
      return transport.fd;
    }

  pthread_mutex_lock(&g_qpk_request_lock);
  owner->fd = transport.fd;
  if (owner->canceled)
    {
      ret = -ECANCELED;
    }
  else
    {
      ret = 0;
    }
  pthread_mutex_unlock(&g_qpk_request_lock);

  if (ret != 0)
    {
      goto out;
    }

  if (tls)
    {
      ret = qpk_tls_open(&transport, host, timeout_ms);
      if (ret != 0)
        {
          goto out;
        }
    }

  request_len = snprintf(request, sizeof(request),
                         "%s %s HTTP/1.0\r\nHost: %s\r\n"
                         "Connection: close\r\nContent-Length: %lu\r\n\r\n",
                         method && method[0] ? method : "GET",
                         path != NULL ? path : "/", host,
                         (unsigned long)body_len);
  if (request_len < 0 || (size_t)request_len >= sizeof(request))
    {
      ret = -E2BIG;
      goto out;
    }

  ret = qpk_transport_write_all(&transport, request, request_len);
  if (ret == 0 && body_len > 0)
    {
      ret = qpk_transport_write_all(&transport, body, body_len);
    }

  if (ret != 0)
    {
      goto out;
    }

  buf = malloc(QPK_HTTP_MAX_BODY + 1);
  if (buf == NULL)
    {
      ret = -ENOMEM;
      goto out;
    }

  while (used < QPK_HTTP_MAX_BODY)
    {
      ssize_t n = qpk_transport_read(&transport, buf + used,
                                     QPK_HTTP_MAX_BODY - used);
      if (n < 0)
        {
          ret = (int)n;
          goto out;
        }

      if (n == 0)
        {
          break;
        }

      used += (size_t)n;
    }

  buf[used] = '\0';
  p = strstr(buf, "HTTP/");
  if (p == NULL || sscanf(p, "HTTP/%*s %d", status) != 1)
    {
      ret = -EPROTO;
      goto out;
    }

  p = strstr(buf, "\r\n\r\n");
  if (p == NULL)
    {
      ret = -EPROTO;
      goto out;
    }

  p += 4;
  memmove(buf, p, used - (size_t)(p - buf));
  used -= (size_t)(p - buf);
  buf[used] = '\0';
  *response = buf;
  buf = NULL;
  ret = 0;

out:
  pthread_mutex_lock(&g_qpk_request_lock);
  owner->fd = -1;
  pthread_mutex_unlock(&g_qpk_request_lock);
  qpk_transport_close(&transport);
  free(buf);
  return ret;
}

static void qpk_request_free(struct qpk_request_s *request)
{
  free(request->url);
  free(request->method);
  free(request->body);
  free(request->response);
  free(request);
}

static void qpk_request_unlink_locked(struct qpk_request_s *request)
{
  struct qpk_request_s **link = &g_qpk_requests;

  while (*link != NULL)
    {
      if (*link == request)
        {
          *link = request->next;
          request->next = NULL;
          return;
        }

      link = &(*link)->next;
    }
}

static void qpk_reject(JSContext *ctx, JSValueConst reject, int error_code)
{
  char message[80];
  JSValue error;
  JSValue result;

  snprintf(message, sizeof(message), "Network request failed (%d)",
           error_code);
  error = JS_NewError(ctx);
  JS_SetPropertyStr(ctx, error, "code", JS_NewInt32(ctx, error_code));
  JS_SetPropertyStr(ctx, error, "message", JS_NewString(ctx, message));
  result = JS_Call(ctx, reject, JS_UNDEFINED, 1, &error);
  JS_FreeValue(ctx, result);
  JS_FreeValue(ctx, error);
}

static void qpk_request_settle(struct qpk_request_s *request)
{
  JSContext *ctx = request->ctx;
  JSValue call_result;

  if (request->result == 0)
    {
      JSValue result = JS_NewObject(ctx);

      JS_SetPropertyStr(ctx, result, "code",
                        JS_NewInt32(ctx, request->status));
      JS_SetPropertyStr(ctx, result, "data",
                        JS_NewString(ctx, request->response != NULL ?
                                    request->response : ""));
      call_result = JS_Call(ctx, request->resolve, JS_UNDEFINED, 1, &result);
      JS_FreeValue(ctx, call_result);
      JS_FreeValue(ctx, result);
    }
  else
    {
      qpk_reject(ctx, request->reject, request->result);
    }

  JS_FreeValue(ctx, request->resolve);
  JS_FreeValue(ctx, request->reject);
  qpk_request_free(request);
}

static void *qpk_request_worker(void *arg)
{
  struct qpk_request_s *request = arg;
  bool release;

  request->result = qpk_http_request(request, request->url,
                                     request->method, request->body,
                                     request->timeout, &request->status,
                                     &request->response);

  pthread_mutex_lock(&g_qpk_request_lock);
  request->done = true;
  release = request->canceled;
  if (release)
    {
      qpk_request_unlink_locked(request);
    }
  pthread_mutex_unlock(&g_qpk_request_lock);

  if (release)
    {
      qpk_request_free(request);
    }

  return NULL;
}

static int qpk_request_start(struct qpk_request_s *request)
{
  struct qpk_request_s *cursor;
  pthread_attr_t attr;
  pthread_t thread;
  int active = 0;
  int ret;

  ret = pthread_attr_init(&attr);
  if (ret != 0)
    {
      return -ret;
    }

  ret = pthread_attr_setstacksize(&attr, QPK_HTTP_WORKER_STACK);
  if (ret == 0)
    {
      ret = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    }

  if (ret != 0)
    {
      pthread_attr_destroy(&attr);
      return -ret;
    }

  pthread_mutex_lock(&g_qpk_request_lock);
  for (cursor = g_qpk_requests; cursor != NULL; cursor = cursor->next)
    {
      active++;
    }

  if (active >= QPK_HTTP_MAX_REQUESTS)
    {
      pthread_mutex_unlock(&g_qpk_request_lock);
      pthread_attr_destroy(&attr);
      return -EBUSY;
    }

  request->next = g_qpk_requests;
  g_qpk_requests = request;
  pthread_mutex_unlock(&g_qpk_request_lock);

  ret = pthread_create(&thread, &attr, qpk_request_worker, request);
  pthread_attr_destroy(&attr);
  if (ret != 0)
    {
      pthread_mutex_lock(&g_qpk_request_lock);
      qpk_request_unlink_locked(request);
      pthread_mutex_unlock(&g_qpk_request_lock);
      return -ret;
    }

  return 0;
}

static JSValue qpk_fetch(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv)
{
  struct qpk_request_s *request;
  JSValue resolving[2];
  JSValue promise;
  JSValue opts;
  JSValue v;
  const char *url = NULL;
  const char *method = "GET";
  bool method_owned = false;
  const char *body = NULL;
  bool has_body;
  bool has_url;
  int32_t timeout = 15000;
  int ret;

  (void)this_val;
  promise = JS_NewPromiseCapability(ctx, resolving);
  if (JS_IsException(promise))
    {
      return promise;
    }

  opts = argc > 0 ? argv[0] : JS_UNDEFINED;
  if (JS_IsString(opts))
    {
      url = JS_ToCString(ctx, opts);
    }
  else if (JS_IsObject(opts))
    {
      v = JS_GetPropertyStr(ctx, opts, "url");
      if (!JS_IsUndefined(v))
        {
          url = JS_ToCString(ctx, v);
        }
      JS_FreeValue(ctx, v);

      v = JS_GetPropertyStr(ctx, opts, "method");
      if (!JS_IsUndefined(v))
        {
          method = JS_ToCString(ctx, v);
          method_owned = method != NULL;
        }
      JS_FreeValue(ctx, v);

      v = JS_GetPropertyStr(ctx, opts, "data");
      if (!JS_IsUndefined(v))
        {
          body = JS_ToCString(ctx, v);
        }
      JS_FreeValue(ctx, v);

      v = JS_GetPropertyStr(ctx, opts, "timeout");
      if (!JS_IsUndefined(v))
        {
          JS_ToInt32(ctx, &timeout, v);
        }
      JS_FreeValue(ctx, v);
    }

  has_url = url != NULL;
  has_body = body != NULL;
  request = calloc(1, sizeof(*request));
  if (request != NULL)
    {
      request->ctx = ctx;
      request->resolve = resolving[0];
      request->reject = resolving[1];
      request->timeout = timeout;
      request->fd = -1;
      request->url = has_url ? strdup(url) : NULL;
      request->method = method != NULL ? strdup(method) : NULL;
      request->body = has_body ? strdup(body) : NULL;
    }

  if (has_url)
    {
      JS_FreeCString(ctx, url);
    }
  if (method_owned)
    {
      JS_FreeCString(ctx, method);
    }
  if (has_body)
    {
      JS_FreeCString(ctx, body);
    }

  if (request == NULL)
    {
      ret = -ENOMEM;
    }
  else if (request->url == NULL || request->method == NULL ||
           (has_body && request->body == NULL))
    {
      ret = !has_url ? -EINVAL : -ENOMEM;
    }
  else
    {
      ret = qpk_request_start(request);
    }

  if (ret != 0)
    {
      qpk_reject(ctx, resolving[1], ret);
      JS_FreeValue(ctx, resolving[0]);
      JS_FreeValue(ctx, resolving[1]);
      if (request != NULL)
        {
          qpk_request_free(request);
        }
    }

  return promise;
}

int qpk_net_poll(JSContext *ctx)
{
  struct qpk_request_s **link;
  struct qpk_request_s *request;
  int completed = 0;

  for (; ; )
    {
      pthread_mutex_lock(&g_qpk_request_lock);
      link = &g_qpk_requests;
      while (*link != NULL &&
             (!(*link)->done || (*link)->ctx != ctx))
        {
          link = &(*link)->next;
        }

      request = *link;
      if (request != NULL)
        {
          *link = request->next;
          request->next = NULL;
        }
      pthread_mutex_unlock(&g_qpk_request_lock);

      if (request == NULL)
        {
          break;
        }

      qpk_request_settle(request);
      completed++;
    }

  return completed;
}

void qpk_net_cancel(JSContext *ctx)
{
  JSValue callbacks[QPK_HTTP_MAX_REQUESTS * 2];
  struct qpk_request_s *release = NULL;
  struct qpk_request_s **link;
  struct qpk_request_s *request;
  int callback_count = 0;
  int i;

  pthread_mutex_lock(&g_qpk_request_lock);
  link = &g_qpk_requests;
  while (*link != NULL)
    {
      request = *link;
      if (request->ctx != ctx)
        {
          link = &request->next;
          continue;
        }

      callbacks[callback_count++] = request->resolve;
      callbacks[callback_count++] = request->reject;
      request->resolve = JS_UNDEFINED;
      request->reject = JS_UNDEFINED;
      request->ctx = NULL;
      request->canceled = true;
      if (request->fd >= 0)
        {
          (void)shutdown(request->fd, SHUT_RDWR);
        }

      *link = request->next;
      request->next = NULL;
      if (request->done)
        {
          request->next = release;
          release = request;
        }
    }
  pthread_mutex_unlock(&g_qpk_request_lock);

  for (i = 0; i < callback_count; i++)
    {
      JS_FreeValue(ctx, callbacks[i]);
    }

  while (release != NULL)
    {
      request = release;
      release = release->next;
      qpk_request_free(request);
    }
}

int qpk_net_install(JSContext *ctx)
{
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue system = JS_GetPropertyStr(ctx, global, "system");
  JSValue network = JS_NewObject(ctx);
  JSValue fetch = JS_NewObject(ctx);

  if (!JS_IsObject(system))
    {
      system = JS_NewObject(ctx);
    }
  JS_SetPropertyStr(ctx, network, "status",
                    JS_NewCFunction(ctx, qpk_network_status, "status", 0));
  JS_SetPropertyStr(ctx, network, "getType",
                    JS_NewCFunction(ctx, qpk_network_get_type, "getType", 0));
  JS_SetPropertyStr(ctx, system, "network", network);
  JS_SetPropertyStr(ctx, fetch, "fetch",
                    JS_NewCFunction(ctx, qpk_fetch, "fetch", 1));
  JS_SetPropertyStr(ctx, system, "fetch", fetch);
  JS_SetPropertyStr(ctx, global, "system", system);
  JS_FreeValue(ctx, global);
  return 0;
}
