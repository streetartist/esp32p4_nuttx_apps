#include <nuttx/config.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <netutils/netlib.h>
#include "quickjs.h"
#include "qpk_net.h"

#define QPK_HTTP_MAX_BODY  (128 * 1024)

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

static int qpk_http_request(const char *url, const char *method,
                            const char *body, int timeout_ms,
                            int *status, char **response)
{
  const char *p;
  const char *host_start;
  const char *path;
  char host[128];
  char port[8] = "80";
  char request[1024];
  struct addrinfo hints;
  struct addrinfo *res = NULL;
  struct timeval tv;
  int fd = -1;
  int ret = -EIO;
  size_t host_len;
  size_t body_len = body ? strlen(body) : 0;
  size_t used = 0;
  char *buf = NULL;

  if (strncmp(url, "http://", 7) != 0)
    {
      return -EPROTONOSUPPORT;
    }

  host_start = url + 7;
  path = strchr(host_start, '/');
  if (path == NULL)
    {
      path = host_start + strlen(host_start);
    }
  host_len = (size_t)(path - host_start);
  if (host_len == 0 || host_len >= sizeof(host))
    {
      return -EINVAL;
    }
  memcpy(host, host_start, host_len);
  host[host_len] = '\0';
  p = strchr(host, ':');
  if (p != NULL)
    {
      size_t n = strlen(p + 1);
      if (n == 0 || n >= sizeof(port))
        {
          return -EINVAL;
        }
      memcpy(port, p + 1, n + 1);
      host[p - host] = '\0';
    }

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
      ret = -errno;
      goto out;
    }
  tv.tv_sec = timeout_ms > 0 ? timeout_ms / 1000 : 15;
  tv.tv_usec = (timeout_ms > 0 ? timeout_ms % 1000 : 0) * 1000;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  if (connect(fd, res->ai_addr, res->ai_addrlen) < 0)
    {
      ret = -errno;
      goto out;
    }

  snprintf(request, sizeof(request),
           "%s %.*s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n"
           "Content-Length: %lu\r\n\r\n",
           method && method[0] ? method : "GET",
           (int)(path - host_start), path, host,
           (unsigned long)body_len);
  if (send(fd, request, strlen(request), 0) < 0 ||
      (body_len > 0 && send(fd, body, body_len, 0) < 0))
    {
      ret = -errno;
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
      ssize_t n = recv(fd, buf + used, QPK_HTTP_MAX_BODY - used, 0);
      if (n <= 0)
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
  if (fd >= 0)
    {
      close(fd);
    }
  if (res != NULL)
    {
      freeaddrinfo(res);
    }
  free(buf);
  return ret;
}

static JSValue qpk_fetch(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv)
{
  JSValue resolving[2];
  JSValue promise;
  JSValue opts;
  JSValue v;
  const char *url = NULL;
  const char *method = "GET";
  int method_owned = 0;
  const char *body = NULL;
  int32_t timeout = 15000;
  int status = 0;
  char *response = NULL;
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
      url = JS_ToCString(ctx, v);
      JS_FreeValue(ctx, v);
      v = JS_GetPropertyStr(ctx, opts, "method");
      if (!JS_IsUndefined(v))
        {
          method = JS_ToCString(ctx, v);
          method_owned = method != NULL;
        }
      JS_FreeValue(ctx, v);
      v = JS_GetPropertyStr(ctx, opts, "data");
      if (!JS_IsUndefined(v)) body = JS_ToCString(ctx, v);
      JS_FreeValue(ctx, v);
      v = JS_GetPropertyStr(ctx, opts, "timeout");
      if (!JS_IsUndefined(v)) JS_ToInt32(ctx, &timeout, v);
      JS_FreeValue(ctx, v);
    }
  ret = url ? qpk_http_request(url, method, body, timeout, &status, &response)
            : -EINVAL;
  if (ret == 0)
    {
      JSValue result = JS_NewObject(ctx);
      JS_SetPropertyStr(ctx, result, "code", JS_NewInt32(ctx, status));
      JS_SetPropertyStr(ctx, result, "data", JS_NewString(ctx, response));
      JS_Call(ctx, resolving[0], JS_UNDEFINED, 1, &result);
      JS_FreeValue(ctx, result);
      free(response);
    }
  else
    {
      JSValue error = JS_NewError(ctx);
      JS_SetPropertyStr(ctx, error, "code", JS_NewInt32(ctx, ret));
      JS_SetPropertyStr(ctx, error, "message",
                       JS_NewString(ctx, ret == -EPROTONOSUPPORT ?
                                    "HTTPS is not enabled" : "HTTP request failed"));
      JS_Call(ctx, resolving[1], JS_UNDEFINED, 1, &error);
      JS_FreeValue(ctx, error);
    }
  if (url) JS_FreeCString(ctx, url);
  if (method_owned) JS_FreeCString(ctx, method);
  if (body) JS_FreeCString(ctx, body);
  JS_FreeValue(ctx, resolving[0]);
  JS_FreeValue(ctx, resolving[1]);
  return promise;
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
