/****************************************************************************
 * apps/system/desktop/qpk_runtime.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <lvgl/lvgl.h>
#include <quickjs.h>

#include "qpk_runtime.h"
#include "qpk_net.h"

#define QPK_MEMORY_LIMIT  (2 * 1024 * 1024)
#define QPK_STACK_LIMIT   (16 * 1024)
#define QPK_MAX_WIDGETS   32
#define QPK_MAX_EVENTS    16
#define QPK_MAX_TIMERS    8
#define QPK_EVAL_BUDGET   250
#define QPK_EVENT_BUDGET  80

struct qpk_event_s
{
  JSValue function;
  bool used;
};

struct qpk_timer_s
{
  JSValue function;
  lv_timer_t *timer;
  int id;
  bool used;
};

struct qpk_runtime_s
{
  JSRuntime *runtime;
  JSContext *context;
  lv_obj_t *root;
  lv_obj_t *widgets[QPK_MAX_WIDGETS];
  struct qpk_event_s events[QPK_MAX_EVENTS];
  struct qpk_timer_s timers[QPK_MAX_TIMERS];
  qpk_font_cb_t font_cb;
  qpk_message_cb_t toast_cb;
  qpk_message_cb_t dialog_cb;
  char name[48];
  char package[48];
  char version[24];
  char error[160];
  uint64_t deadline_ms;
  uint32_t primary_color;
  uint32_t secondary_color;
  uint32_t surface_color;
  int next_timer_id;
};

static struct qpk_runtime_s g_qpk;

static char *qpk_module_normalize(JSContext *ctx, const char *base,
                                  const char *name, void *opaque)
{
  const char *slash;
  char path[256];
  (void)ctx;
  (void)opaque;
  if (name[0] != '.' || base == NULL)
    {
      return strdup(name);
    }
  slash = strrchr(base, '/');
  if (slash == NULL)
    {
      return strdup(name);
    }
  snprintf(path, sizeof(path), "%.*s/%s", (int)(slash - base), base, name);
  return strdup(path);
}

static JSModuleDef *qpk_module_loader(JSContext *ctx, const char *name,
                                      void *opaque)
{
  const char *builtin = NULL;
  char *source = NULL;
  size_t size;
  FILE *fp;
  JSValue value;
  (void)opaque;

  if (strcmp(name, "@system.fetch") == 0)
    {
      builtin = "export default globalThis.system.fetch;";
    }
  else if (strcmp(name, "@system.network") == 0)
    {
      builtin = "export default globalThis.system.network;";
    }
  if (builtin != NULL)
    {
      value = JS_Eval(ctx, builtin, strlen(builtin), name,
                      JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
      return JS_IsException(value) ? NULL : JS_VALUE_GET_PTR(value);
    }

  fp = fopen(name, "rb");
  if (fp == NULL)
    {
      return NULL;
    }
  fseek(fp, 0, SEEK_END);
  size = (size_t)ftell(fp);
  fseek(fp, 0, SEEK_SET);
  source = malloc(size + 1);
  if (source == NULL || fread(source, 1, size, fp) != size)
    {
      fclose(fp);
      free(source);
      return NULL;
    }
  fclose(fp);
  source[size] = '\0';
  value = JS_Eval(ctx, source, size, name,
                  JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
  free(source);
  return JS_IsException(value) ? NULL : JS_VALUE_GET_PTR(value);
}

static uint64_t qpk_now_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int qpk_interrupt(JSRuntime *runtime, void *opaque)
{
  struct qpk_runtime_s *qpk = opaque;

  (void)runtime;
  return qpk->deadline_ms != 0 && qpk_now_ms() > qpk->deadline_ms;
}

static void qpk_deadline_begin(unsigned int budget_ms)
{
  g_qpk.deadline_ms = qpk_now_ms() + budget_ms;
  JS_UpdateStackTop(g_qpk.runtime);
}

static void qpk_deadline_end(void)
{
  g_qpk.deadline_ms = 0;
}

static void qpk_show_error(const char *prefix)
{
  JSValue exception;
  JSValue stack;
  const char *message;
  const char *trace;

  exception = JS_GetException(g_qpk.context);
  message = JS_ToCString(g_qpk.context, exception);
  snprintf(g_qpk.error, sizeof(g_qpk.error), "%s%s%s",
           prefix ? prefix : "JavaScript 错误",
           message ? ": " : "", message ? message : "未知异常");
  stack = JS_GetPropertyStr(g_qpk.context, exception, "stack");
  trace = JS_IsUndefined(stack) ? NULL :
          JS_ToCString(g_qpk.context, stack);
  printf("[qpk] %s\n", trace ? trace : g_qpk.error);
  if (g_qpk.toast_cb != NULL)
    {
      g_qpk.toast_cb(g_qpk.error);
    }

  if (trace != NULL)
    {
      JS_FreeCString(g_qpk.context, trace);
    }

  JS_FreeValue(g_qpk.context, stack);
  if (message != NULL)
    {
      JS_FreeCString(g_qpk.context, message);
    }

  JS_FreeValue(g_qpk.context, exception);
}

static int qpk_run_jobs(void)
{
  JSContext *context;
  int ret;

  do
    {
      ret = JS_ExecutePendingJob(g_qpk.runtime, &context);
    }
  while (ret > 0);

  if (ret < 0)
    {
      qpk_show_error("异步任务错误");
      return -1;
    }

  return 0;
}

static JSValue qpk_call(JSValueConst function, unsigned int budget_ms)
{
  JSValue result;

  qpk_deadline_begin(budget_ms);
  result = JS_Call(g_qpk.context, function, JS_UNDEFINED, 0, NULL);
  qpk_deadline_end();
  if (JS_IsException(result))
    {
      qpk_show_error("事件执行错误");
      return result;
    }

  qpk_deadline_begin(budget_ms);
  qpk_run_jobs();
  qpk_deadline_end();
  return result;
}

static int qpk_add_widget(lv_obj_t *object)
{
  int i;

  for (i = 0; i < QPK_MAX_WIDGETS; i++)
    {
      if (g_qpk.widgets[i] == NULL)
        {
          g_qpk.widgets[i] = object;
          return i + 1;
        }
    }

  return 0;
}

static int qpk_arg_int(JSContext *context, int argc,
                       JSValueConst *argv, int index, int fallback)
{
  int32_t value;

  if (index >= argc || JS_ToInt32(context, &value, argv[index]) < 0)
    {
      return fallback;
    }

  return value;
}

static uint32_t qpk_arg_color(JSContext *context, int argc,
                              JSValueConst *argv, int index,
                              uint32_t fallback)
{
  uint32_t value;

  if (index >= argc || JS_ToUint32(context, &value, argv[index]) < 0)
    {
      return fallback;
    }

  return value;
}

static const char *qpk_arg_string(JSContext *context, int argc,
                                  JSValueConst *argv, int index)
{
  return index < argc ? JS_ToCString(context, argv[index]) : NULL;
}

static void qpk_event_clicked(lv_event_t *event)
{
  struct qpk_event_s *binding = lv_event_get_user_data(event);
  JSValue result;

  if (g_qpk.context == NULL || binding == NULL || !binding->used)
    {
      return;
    }

  result = qpk_call(binding->function, QPK_EVENT_BUDGET);
  JS_FreeValue(g_qpk.context, result);
}

static JSValue js_ui_text(JSContext *context, JSValueConst this_value,
                          int argc, JSValueConst *argv)
{
  const char *text;
  lv_obj_t *label;
  int size;
  int handle;

  (void)this_value;
  text = qpk_arg_string(context, argc, argv, 0);
  if (text == NULL)
    {
      return JS_EXCEPTION;
    }

  size = qpk_arg_int(context, argc, argv, 3, 20);
  label = lv_label_create(g_qpk.root);
  lv_label_set_text(label, text);
  lv_obj_set_pos(label, qpk_arg_int(context, argc, argv, 1, 24),
                 qpk_arg_int(context, argc, argv, 2, 80));
  lv_obj_set_style_text_color(label,
      lv_color_hex(qpk_arg_color(context, argc, argv, 4, 0xffffff)), 0);
  if (g_qpk.font_cb != NULL)
    {
      lv_obj_set_style_text_font(label, g_qpk.font_cb(size), 0);
    }

  handle = qpk_add_widget(label);
  JS_FreeCString(context, text);
  if (handle == 0)
    {
      lv_obj_delete(label);
      return JS_ThrowInternalError(context, "too many widgets");
    }

  return JS_NewInt32(context, handle);
}

static JSValue js_ui_set_text(JSContext *context,
                              JSValueConst this_value,
                              int argc, JSValueConst *argv)
{
  int handle;
  const char *text;

  (void)this_value;
  handle = qpk_arg_int(context, argc, argv, 0, 0);
  text = qpk_arg_string(context, argc, argv, 1);
  if (text == NULL)
    {
      return JS_EXCEPTION;
    }

  if (handle <= 0 || handle > QPK_MAX_WIDGETS ||
      g_qpk.widgets[handle - 1] == NULL)
    {
      JS_FreeCString(context, text);
      return JS_ThrowRangeError(context, "invalid widget handle");
    }

  lv_label_set_text(g_qpk.widgets[handle - 1], text);
  JS_FreeCString(context, text);
  return JS_UNDEFINED;
}

static JSValue js_ui_button(JSContext *context, JSValueConst this_value,
                            int argc, JSValueConst *argv)
{
  const char *text;
  lv_obj_t *button;
  lv_obj_t *label;
  struct qpk_event_s *binding = NULL;
  int handle;
  int i;
  uint32_t button_color;
  uint32_t text_color;
  unsigned int brightness;

  (void)this_value;
  text = qpk_arg_string(context, argc, argv, 0);
  if (text == NULL)
    {
      return JS_EXCEPTION;
    }

  if (argc < 6 || !JS_IsFunction(context, argv[5]))
    {
      JS_FreeCString(context, text);
      return JS_ThrowTypeError(context, "button handler must be a function");
    }

  for (i = 0; i < QPK_MAX_EVENTS; i++)
    {
      if (!g_qpk.events[i].used)
        {
          binding = &g_qpk.events[i];
          break;
        }
    }

  if (binding == NULL)
    {
      JS_FreeCString(context, text);
      return JS_ThrowInternalError(context, "too many event handlers");
    }

  button = lv_button_create(g_qpk.root);
  lv_obj_set_pos(button, qpk_arg_int(context, argc, argv, 1, 220),
                 qpk_arg_int(context, argc, argv, 2, 160));
  lv_obj_set_size(button, qpk_arg_int(context, argc, argv, 3, 300),
                  qpk_arg_int(context, argc, argv, 4, 54));
  button_color = qpk_arg_color(context, argc, argv, 6, 0x6677f5);
  brightness = ((button_color >> 16) & 0xff) +
               ((button_color >> 8) & 0xff) + (button_color & 0xff);
  text_color = brightness > 510 ? 0x172033 : 0xffffff;
  lv_obj_set_style_bg_color(button, lv_color_hex(button_color), 0);
  lv_obj_set_style_radius(button, 14, 0);
  label = lv_label_create(button);
  lv_label_set_text(label, text);
  lv_obj_set_style_text_color(label, lv_color_hex(text_color), 0);
  if (g_qpk.font_cb != NULL)
    {
      lv_obj_set_style_text_font(label, g_qpk.font_cb(20), 0);
    }

  lv_obj_center(label);
  binding->function = JS_DupValue(context, argv[5]);
  binding->used = true;
  lv_obj_add_event_cb(button, qpk_event_clicked, LV_EVENT_CLICKED, binding);
  handle = qpk_add_widget(button);
  JS_FreeCString(context, text);
  if (handle == 0)
    {
      JS_FreeValue(context, binding->function);
      binding->used = false;
      lv_obj_delete(button);
      return JS_ThrowInternalError(context, "too many widgets");
    }

  return JS_NewInt32(context, handle);
}

static const char *qpk_message_arg(JSContext *context, int argc,
                                   JSValueConst *argv, JSValue *holder)
{
  if (argc < 1)
    {
      return NULL;
    }

  if (JS_IsObject(argv[0]))
    {
      *holder = JS_GetPropertyStr(context, argv[0], "message");
      return JS_ToCString(context, *holder);
    }

  *holder = JS_UNDEFINED;
  return JS_ToCString(context, argv[0]);
}

static JSValue js_prompt_toast(JSContext *context,
                               JSValueConst this_value,
                               int argc, JSValueConst *argv)
{
  JSValue holder;
  const char *message;

  (void)this_value;
  message = qpk_message_arg(context, argc, argv, &holder);
  if (message == NULL)
    {
      return JS_EXCEPTION;
    }

  if (g_qpk.toast_cb != NULL)
    {
      g_qpk.toast_cb(message);
    }

  JS_FreeCString(context, message);
  JS_FreeValue(context, holder);
  return JS_UNDEFINED;
}

static JSValue js_prompt_dialog(JSContext *context,
                                JSValueConst this_value,
                                int argc, JSValueConst *argv)
{
  JSValue title_value = JS_UNDEFINED;
  JSValue message_value = JS_UNDEFINED;
  const char *title = NULL;
  const char *message = NULL;
  char text[256];

  (void)this_value;
  if (argc > 0 && JS_IsObject(argv[0]))
    {
      title_value = JS_GetPropertyStr(context, argv[0], "title");
      message_value = JS_GetPropertyStr(context, argv[0], "message");
      title = JS_ToCString(context, title_value);
      message = JS_ToCString(context, message_value);
    }
  else if (argc > 0)
    {
      message = JS_ToCString(context, argv[0]);
    }

  snprintf(text, sizeof(text), "%s%s%s", title ? title : "快应用",
           message ? "\n" : "", message ? message : "");
  if (g_qpk.dialog_cb != NULL)
    {
      g_qpk.dialog_cb(text);
    }

  if (title != NULL)
    {
      JS_FreeCString(context, title);
    }

  if (message != NULL)
    {
      JS_FreeCString(context, message);
    }

  JS_FreeValue(context, title_value);
  JS_FreeValue(context, message_value);
  return JS_UNDEFINED;
}

static void qpk_timer_cb(lv_timer_t *timer)
{
  struct qpk_timer_s *binding = lv_timer_get_user_data(timer);
  JSValue result;

  if (g_qpk.context == NULL || binding == NULL || !binding->used)
    {
      return;
    }

  result = qpk_call(binding->function, QPK_EVENT_BUDGET);
  if (JS_IsException(result))
    {
      JS_FreeValue(g_qpk.context, binding->function);
      binding->used = false;
      binding->timer = NULL;
      lv_timer_delete(timer);
    }

  JS_FreeValue(g_qpk.context, result);
}

static JSValue js_set_interval(JSContext *context,
                               JSValueConst this_value,
                               int argc, JSValueConst *argv)
{
  struct qpk_timer_s *binding = NULL;
  int interval;
  int i;

  (void)this_value;
  if (argc < 1 || !JS_IsFunction(context, argv[0]))
    {
      return JS_ThrowTypeError(context, "callback must be a function");
    }

  interval = qpk_arg_int(context, argc, argv, 1, 1000);
  if (interval < 20)
    {
      interval = 20;
    }

  for (i = 0; i < QPK_MAX_TIMERS; i++)
    {
      if (!g_qpk.timers[i].used)
        {
          binding = &g_qpk.timers[i];
          break;
        }
    }

  if (binding == NULL)
    {
      return JS_ThrowInternalError(context, "too many timers");
    }

  binding->id = ++g_qpk.next_timer_id;
  binding->function = JS_DupValue(context, argv[0]);
  binding->used = true;
  binding->timer = lv_timer_create(qpk_timer_cb, interval, binding);
  if (binding->timer == NULL)
    {
      JS_FreeValue(context, binding->function);
      binding->used = false;
      return JS_ThrowInternalError(context, "cannot create timer");
    }

  return JS_NewInt32(context, binding->id);
}

static JSValue js_clear_interval(JSContext *context,
                                 JSValueConst this_value,
                                 int argc, JSValueConst *argv)
{
  int id;
  int i;

  (void)this_value;
  id = qpk_arg_int(context, argc, argv, 0, 0);
  for (i = 0; i < QPK_MAX_TIMERS; i++)
    {
      struct qpk_timer_s *binding = &g_qpk.timers[i];

      if (binding->used && binding->id == id)
        {
          lv_timer_delete(binding->timer);
          JS_FreeValue(context, binding->function);
          binding->timer = NULL;
          binding->used = false;
          break;
        }
    }

  return JS_UNDEFINED;
}

static JSValue js_app_get_info(JSContext *context,
                               JSValueConst this_value,
                               int argc, JSValueConst *argv)
{
  JSValue info;

  (void)this_value;
  (void)argc;
  (void)argv;
  info = JS_NewObject(context);
  JS_SetPropertyStr(context, info, "name",
                    JS_NewString(context, g_qpk.name));
  JS_SetPropertyStr(context, info, "packageName",
                    JS_NewString(context, g_qpk.package));
  JS_SetPropertyStr(context, info, "versionName",
                    JS_NewString(context, g_qpk.version));
  return info;
}

static JSValue js_console_log(JSContext *context,
                              JSValueConst this_value,
                              int argc, JSValueConst *argv)
{
  int i;

  (void)this_value;
  printf("[qpk]");
  for (i = 0; i < argc; i++)
    {
      const char *text = JS_ToCString(context, argv[i]);
      if (text == NULL)
        {
          return JS_EXCEPTION;
        }

      printf(" %s", text);
      JS_FreeCString(context, text);
    }

  printf("\n");
  return JS_UNDEFINED;
}

static void qpk_install_api(JSContext *context)
{
  JSValue global;
  JSValue object;

  global = JS_GetGlobalObject(context);
  object = JS_NewObject(context);
  JS_SetPropertyStr(context, object, "text",
                    JS_NewCFunction(context, js_ui_text, "text", 5));
  JS_SetPropertyStr(context, object, "setText",
                    JS_NewCFunction(context, js_ui_set_text, "setText", 2));
  JS_SetPropertyStr(context, object, "button",
                    JS_NewCFunction(context, js_ui_button, "button", 7));
  JS_SetPropertyStr(context, object, "primary",
                    JS_NewUint32(context, g_qpk.primary_color));
  JS_SetPropertyStr(context, object, "secondary",
                    JS_NewUint32(context, g_qpk.secondary_color));
  JS_SetPropertyStr(context, object, "surface",
                    JS_NewUint32(context, g_qpk.surface_color));
  JS_SetPropertyStr(context, global, "ui", object);

  object = JS_NewObject(context);
  JS_SetPropertyStr(context, object, "showToast",
                    JS_NewCFunction(context, js_prompt_toast,
                                    "showToast", 1));
  JS_SetPropertyStr(context, object, "dialog",
                    JS_NewCFunction(context, js_prompt_dialog,
                                    "dialog", 1));
  JS_SetPropertyStr(context, global, "prompt", object);

  object = JS_NewObject(context);
  JS_SetPropertyStr(context, object, "getInfo",
                    JS_NewCFunction(context, js_app_get_info,
                                    "getInfo", 0));
  JS_SetPropertyStr(context, global, "app", object);

  object = JS_NewObject(context);
  JS_SetPropertyStr(context, object, "log",
                    JS_NewCFunction(context, js_console_log, "log", 1));
  JS_SetPropertyStr(context, global, "console", object);
  JS_SetPropertyStr(context, global, "setInterval",
                    JS_NewCFunction(context, js_set_interval,
                                    "setInterval", 2));
  JS_SetPropertyStr(context, global, "clearInterval",
                    JS_NewCFunction(context, js_clear_interval,
                                    "clearInterval", 1));
  JS_FreeValue(context, global);
}

int qpk_runtime_launch(lv_obj_t *root, const char *name,
                       const char *package, const char *version,
                       const char *filename, const char *source,
                       size_t source_len, qpk_font_cb_t font_cb,
                       qpk_message_cb_t toast_cb,
                       qpk_message_cb_t dialog_cb)
{
  JSValue result;
  uint32_t background;
  unsigned int brightness;

  if (root == NULL || source == NULL || source_len == 0)
    {
      return -EINVAL;
    }

  qpk_runtime_stop();
  memset(&g_qpk, 0, sizeof(g_qpk));
  g_qpk.root = root;
  g_qpk.font_cb = font_cb;
  g_qpk.toast_cb = toast_cb;
  g_qpk.dialog_cb = dialog_cb;
  g_qpk.next_timer_id = 100;
  background = lv_color_to_u32(lv_obj_get_style_bg_color(root,
                                                          LV_PART_MAIN));
  brightness = ((background >> 16) & 0xff) +
               ((background >> 8) & 0xff) + (background & 0xff);
  if (brightness > 384)
    {
      g_qpk.primary_color = 0x172033;
      g_qpk.secondary_color = 0x65708a;
      g_qpk.surface_color = 0xe8ebf2;
    }
  else
    {
      g_qpk.primary_color = 0xffffff;
      g_qpk.secondary_color = 0x8f9bb5;
      g_qpk.surface_color = 0x30394f;
    }
  strlcpy(g_qpk.name, name ? name : "Quick App", sizeof(g_qpk.name));
  strlcpy(g_qpk.package, package ? package : "", sizeof(g_qpk.package));
  strlcpy(g_qpk.version, version ? version : "", sizeof(g_qpk.version));

  g_qpk.runtime = JS_NewRuntime();
  if (g_qpk.runtime == NULL)
    {
      return -ENOMEM;
    }

  JS_SetMemoryLimit(g_qpk.runtime, QPK_MEMORY_LIMIT);
  JS_SetMaxStackSize(g_qpk.runtime, QPK_STACK_LIMIT);
  JS_SetInterruptHandler(g_qpk.runtime, qpk_interrupt, &g_qpk);
  JS_SetModuleLoaderFunc(g_qpk.runtime, qpk_module_normalize,
                         qpk_module_loader, NULL);
  g_qpk.context = JS_NewContext(g_qpk.runtime);
  if (g_qpk.context == NULL)
    {
      qpk_runtime_stop();
      return -ENOMEM;
    }

  qpk_install_api(g_qpk.context);
  qpk_net_install(g_qpk.context);
  qpk_deadline_begin(QPK_EVAL_BUDGET);
  result = JS_Eval(g_qpk.context, source, source_len,
                   filename ? filename : "app.js", JS_EVAL_TYPE_MODULE);
  qpk_deadline_end();
  if (JS_IsException(result))
    {
      qpk_show_error("启动失败");
      JS_FreeValue(g_qpk.context, result);
      qpk_runtime_stop();
      return -ENOEXEC;
    }

  JS_FreeValue(g_qpk.context, result);
  qpk_deadline_begin(QPK_EVAL_BUDGET);
  qpk_run_jobs();
  qpk_deadline_end();
  printf("[qpk] started %s (%s %s) with QuickJS\n",
         g_qpk.name, g_qpk.package, g_qpk.version);
  return 0;
}

void qpk_runtime_stop(void)
{
  int i;

  if (g_qpk.context != NULL)
    {
      for (i = 0; i < QPK_MAX_TIMERS; i++)
        {
          if (g_qpk.timers[i].used)
            {
              lv_timer_delete(g_qpk.timers[i].timer);
              JS_FreeValue(g_qpk.context, g_qpk.timers[i].function);
            }
        }

      for (i = 0; i < QPK_MAX_EVENTS; i++)
        {
          if (g_qpk.events[i].used)
            {
              JS_FreeValue(g_qpk.context, g_qpk.events[i].function);
            }
        }

      JS_FreeContext(g_qpk.context);
    }

  if (g_qpk.runtime != NULL)
    {
      JS_FreeRuntime(g_qpk.runtime);
    }

  memset(&g_qpk, 0, sizeof(g_qpk));
}

bool qpk_runtime_running(void)
{
  return g_qpk.context != NULL;
}
