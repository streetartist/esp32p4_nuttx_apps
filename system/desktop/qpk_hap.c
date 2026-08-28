/****************************************************************************
 * apps/system/desktop/qpk_hap.c
 *
 * HAP/UX shim for Quick App pages: layout, router, canvas 2d.
 ****************************************************************************/

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lvgl/lvgl.h>
#include <quickjs.h>

#include "qpk_hap.h"
#include "qpk_priv.h"

#define HAP_PAGE_STACK  4
#define HAP_PATH_MAX    256

static const char g_qpk_hap_shim[] =
#include "qpk_hap_shim.inc"
;

static char g_hap_stack[HAP_PAGE_STACK][HAP_PATH_MAX];
static char g_hap_pending[HAP_PATH_MAX];
static lv_timer_t *g_hap_nav_timer;
static int g_hap_sp;
static bool g_hap_pending_push;

static void hap_nav_cb(lv_timer_t *timer);

static JSValue js_hap_push(JSContext *context, JSValueConst this_value,
                           int argc, JSValueConst *argv);
static JSValue js_hap_back(JSContext *context, JSValueConst this_value,
                           int argc, JSValueConst *argv);

static bool hap_contains(const char *source, size_t len, const char *pat)
{
  size_t plen;
  size_t i;

  if (source == NULL || pat == NULL)
    {
      return false;
    }

  plen = strlen(pat);
  if (plen == 0 || len < plen)
    {
      return false;
    }

  for (i = 0; i + plen <= len; i++)
    {
      if (memcmp(source + i, pat, plen) == 0)
        {
          return true;
        }
    }

  return false;
}

bool qpk_hap_is_source(const char *source, size_t len)
{
  if (source == NULL || len < 11)
    {
      return false;
    }

  if (memcmp(source, "(function()", 11) == 0)
    {
      return true;
    }

  return hap_contains(source, len, "$app_define$") ||
         hap_contains(source, len, "$app_bootstrap$") ||
         hap_contains(source, len, "createPageHandler");
}

static int hap_bind_router(JSContext *ctx)
{
  JSValue global;
  JSValue system;
  JSValue router;

  global = JS_GetGlobalObject(ctx);
  system = JS_GetPropertyStr(ctx, global, "system");
  if (!JS_IsObject(system))
    {
      JS_FreeValue(ctx, system);
      system = JS_NewObject(ctx);
      JS_SetPropertyStr(ctx, global, "system", JS_DupValue(ctx, system));
    }

  router = JS_GetPropertyStr(ctx, system, "router");
  if (!JS_IsObject(router))
    {
      JS_FreeValue(ctx, router);
      router = JS_NewObject(ctx);
      JS_SetPropertyStr(ctx, system, "router", JS_DupValue(ctx, router));
    }

  JS_SetPropertyStr(ctx, router, "push",
                    JS_NewCFunction(ctx, js_hap_push, "push", 1));
  JS_SetPropertyStr(ctx, router, "back",
                    JS_NewCFunction(ctx, js_hap_back, "back", 0));
  JS_FreeValue(ctx, router);
  JS_FreeValue(ctx, system);
  JS_FreeValue(ctx, global);
  return 0;
}

int qpk_hap_install(JSContext *ctx)
{
  JSValue result;

  if (ctx == NULL)
    {
      return -EINVAL;
    }

  result = JS_Eval(ctx, g_qpk_hap_shim, strlen(g_qpk_hap_shim),
                   "qpk_hap_shim.js", JS_EVAL_TYPE_GLOBAL);
  if (JS_IsException(result))
    {
      qpk_show_error("HAP 垫片失败");
      JS_FreeValue(ctx, result);
      return -ENOEXEC;
    }

  JS_FreeValue(ctx, result);
  return hap_bind_router(ctx);
}

static int hap_uri_to_path(const char *uri, char *out, size_t outlen)
{
  const char *basedir = qpk_basedir();
  const char *u = uri ? uri : "";
  const char *slash;
  const char *comp;

  if (basedir == NULL || basedir[0] == '\0')
    {
      return -EINVAL;
    }

  while (*u == '/')
    {
      u++;
    }

  slash = strrchr(u, '/');
  comp = slash ? slash + 1 : u;
  if (comp[0] == '\0')
    {
      return -EINVAL;
    }

  if (snprintf(out, outlen, "%s/%s/%s.js", basedir, u, comp) >= (int)outlen)
    {
      return -ENAMETOOLONG;
    }

  return 0;
}

static int hap_mount(void)
{
  qpk_run_gc();
  return qpk_call_name_budget("$hap_mount$", 20000);
}

static int hap_load_path(const char *path, bool push)
{
  char *source = NULL;
  size_t len = 0;
  int ret;

  printf("[qpk] hap load %s\n", path);
  qpk_call_name("$hap_teardown$");
  qpk_clear_page();
  ret = qpk_read_file(path, &source, &len);
  if (ret < 0)
    {
      printf("[qpk] hap missing %s (%d)\n", path, ret);
      return ret;
    }

  ret = qpk_eval_global(path, source, len);
  free(source);
  if (ret < 0)
    {
      return ret;
    }

  if (push && g_hap_sp < HAP_PAGE_STACK)
    {
      strlcpy(g_hap_stack[g_hap_sp], path, sizeof(g_hap_stack[0]));
      g_hap_sp++;
    }

  return hap_mount();
}

static void hap_schedule(const char *path, bool push)
{
  strlcpy(g_hap_pending, path, sizeof(g_hap_pending));
  g_hap_pending_push = push;
  if (g_hap_nav_timer != NULL)
    {
      return;
    }

  g_hap_nav_timer = lv_timer_create(hap_nav_cb, 10, NULL);
  if (g_hap_nav_timer != NULL)
    {
      lv_timer_set_repeat_count(g_hap_nav_timer, 1);
    }
}

static void hap_nav_cb(lv_timer_t *timer)
{
  char path[HAP_PATH_MAX];
  bool push = g_hap_pending_push;

  (void)timer;
  g_hap_nav_timer = NULL;
  if (g_hap_pending[0] == '\0')
    {
      return;
    }

  strlcpy(path, g_hap_pending, sizeof(path));
  g_hap_pending[0] = '\0';
  hap_load_path(path, push);
}

static JSValue js_hap_push(JSContext *context, JSValueConst this_value,
                           int argc, JSValueConst *argv)
{
  JSValue uri;
  const char *text;
  char path[HAP_PATH_MAX];
  int ret;

  (void)this_value;
  if (argc < 1 || !JS_IsObject(argv[0]))
    {
      return JS_UNDEFINED;
    }

  uri = JS_GetPropertyStr(context, argv[0], "uri");
  text = JS_ToCString(context, uri);
  JS_FreeValue(context, uri);
  if (text == NULL)
    {
      return JS_UNDEFINED;
    }

  ret = hap_uri_to_path(text, path, sizeof(path));
  printf("[qpk] router.push %s -> %s%s\n", text,
         ret == 0 ? path : "?",
         qpk_in_js_timer() ? " (timer)" : "");
  JS_FreeCString(context, text);
  if (ret == 0)
    {
      hap_schedule(path, true);
    }

  return JS_UNDEFINED;
}

static JSValue js_hap_back(JSContext *context, JSValueConst this_value,
                           int argc, JSValueConst *argv)
{
  (void)context;
  (void)this_value;
  (void)argc;
  (void)argv;

  if (g_hap_sp <= 1)
    {
      printf("[qpk] router.back at root\n");
      return JS_UNDEFINED;
    }

  g_hap_sp--;
  hap_schedule(g_hap_stack[g_hap_sp - 1], false);
  return JS_UNDEFINED;
}

int qpk_hap_eval(const char *filename, const char *source, size_t len)
{
  char path[HAP_PATH_MAX];
  const char *basedir;
  int ret;

  ret = qpk_eval_global(filename, source, len);
  if (ret < 0)
    {
      return ret;
    }

  if (filename != NULL && g_hap_sp < HAP_PAGE_STACK)
    {
      strlcpy(g_hap_stack[g_hap_sp], filename, sizeof(g_hap_stack[0]));
      g_hap_sp++;
    }

  ret = hap_mount();
  if (qpk_widget_count() > 0)
    {
      return ret;
    }

  basedir = qpk_basedir();
  if (basedir == NULL || basedir[0] == '\0')
    {
      return ret;
    }

  if (snprintf(path, sizeof(path), "%s/pages/index/index.js",
               basedir) >= (int)sizeof(path))
    {
      return ret;
    }

  if (filename != NULL && strcmp(filename, path) == 0)
    {
      printf("[qpk] HAP mounted 0 widgets from %s\n",
             filename ? filename : "?");
      return ret;
    }

  printf("[qpk] application only, loading page %s\n", path);
  return hap_load_path(path, true);
}

void qpk_hap_reset(void)
{
  if (g_hap_nav_timer != NULL)
    {
      lv_timer_delete(g_hap_nav_timer);
      g_hap_nav_timer = NULL;
    }

  g_hap_pending[0] = '\0';
  g_hap_pending_push = false;
  g_hap_sp = 0;
}
