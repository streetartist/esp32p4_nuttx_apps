/****************************************************************************
 * apps/system/desktop/qpk_runtime.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <lvgl/lvgl.h>
#include <quickjs.h>

#include "qpk_runtime.h"
#include "qpk_net.h"

#define QPK_MEMORY_LIMIT  (2 * 1024 * 1024)
#define QPK_STACK_LIMIT   (16 * 1024)
#define QPK_MAX_WIDGETS   64
#define QPK_MAX_EVENTS    16
#define QPK_MAX_TIMERS    8
#define QPK_EVAL_BUDGET   1000
#define QPK_EVENT_BUDGET  500
#define QPK_NET_POLL_MS   20
#define QPK_STORAGE_KEY_MAX    64
#define QPK_STORAGE_VALUE_MAX  8192
#define QPK_STORAGE_PATH_MAX   256
#define QPK_STORAGE_ROOT       CONFIG_SYSTEM_DESKTOP_QPK_DIR "/.data"

enum qpk_widget_type_e
{
  QPK_WIDGET_LABEL = 0,
  QPK_WIDGET_NUMBER,
  QPK_WIDGET_PANEL,
  QPK_WIDGET_BUTTON
};

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
  uint8_t widget_types[QPK_MAX_WIDGETS];
  struct qpk_event_s events[QPK_MAX_EVENTS];
  struct qpk_event_s swipe_event;
  struct qpk_timer_s timers[QPK_MAX_TIMERS];
  lv_timer_t *net_timer;
  lv_obj_t *input_shade;
  lv_obj_t *input_textarea;
  JSValue input_callback;
  qpk_font_cb_t font_cb;
  qpk_number_font_cb_t number_font_cb;
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
  else if (strcmp(name, "@system.storage") == 0)
    {
      builtin = "export default globalThis.system.storage;";
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

static void qpk_net_timer_cb(lv_timer_t *timer)
{
  int completed;

  (void)timer;
  if (g_qpk.context == NULL)
    {
      return;
    }

  qpk_deadline_begin(QPK_EVENT_BUDGET);
  completed = qpk_net_poll(g_qpk.context);
  if (completed > 0)
    {
      qpk_run_jobs();
    }
  qpk_deadline_end();
}

static JSValue qpk_call_args(JSValueConst function,
                             unsigned int budget_ms,
                             int argc, JSValueConst *argv)
{
  JSValue result;

  qpk_deadline_begin(budget_ms);
  result = JS_Call(g_qpk.context, function, JS_UNDEFINED, argc, argv);
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

static JSValue qpk_call(JSValueConst function, unsigned int budget_ms)
{
  return qpk_call_args(function, budget_ms, 0, NULL);
}

static int qpk_add_widget(lv_obj_t *object, enum qpk_widget_type_e type)
{
  int i;

  for (i = 0; i < QPK_MAX_WIDGETS; i++)
    {
      if (g_qpk.widgets[i] == NULL)
        {
          g_qpk.widgets[i] = object;
          g_qpk.widget_types[i] = type;
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

static void qpk_event_swiped(lv_event_t *event)
{
  lv_indev_t *indev = lv_event_get_indev(event);
  const char *direction;
  JSValue argument;
  JSValue result;

  if (g_qpk.context == NULL || !g_qpk.swipe_event.used || indev == NULL ||
      g_qpk.input_shade != NULL)
    {
      return;
    }

  switch (lv_indev_get_gesture_dir(indev))
    {
      case LV_DIR_LEFT:
        direction = "left";
        break;
      case LV_DIR_RIGHT:
        direction = "right";
        break;
      case LV_DIR_TOP:
        direction = "up";
        break;
      case LV_DIR_BOTTOM:
        direction = "down";
        break;
      default:
        return;
    }

  argument = JS_NewString(g_qpk.context, direction);
  result = qpk_call_args(g_qpk.swipe_event.function, QPK_EVENT_BUDGET,
                         1, &argument);
  JS_FreeValue(g_qpk.context, argument);
  JS_FreeValue(g_qpk.context, result);
}

static JSValue js_ui_on_swipe(JSContext *context,
                              JSValueConst this_value,
                              int argc, JSValueConst *argv)
{
  (void)this_value;
  if (argc < 1 || !JS_IsFunction(context, argv[0]))
    {
      return JS_ThrowTypeError(context, "swipe handler must be a function");
    }

  if (g_qpk.swipe_event.used)
    {
      JS_FreeValue(context, g_qpk.swipe_event.function);
    }

  g_qpk.swipe_event.function = JS_DupValue(context, argv[0]);
  g_qpk.swipe_event.used = true;
  return JS_UNDEFINED;
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

  handle = qpk_add_widget(label, QPK_WIDGET_LABEL);
  JS_FreeCString(context, text);
  if (handle == 0)
    {
      lv_obj_delete(label);
      return JS_ThrowInternalError(context, "too many widgets");
    }

  return JS_NewInt32(context, handle);
}

static void qpk_number_style(lv_obj_t *label, const char *text)
{
  lv_obj_t *number = lv_obj_get_child(label, 0);
  int digits = strlen(text);

  if (number != NULL)
    {
      lv_label_set_text(number, text);
      if (g_qpk.number_font_cb != NULL)
        {
          lv_obj_set_style_text_font(number,
                                     g_qpk.number_font_cb(digits), 0);
        }

      lv_obj_center(number);
    }
}

static JSValue js_ui_number(JSContext *context, JSValueConst this_value,
                            int argc, JSValueConst *argv)
{
  const char *text;
  lv_obj_t *label;
  int handle;

  (void)this_value;
  text = qpk_arg_string(context, argc, argv, 0);
  if (text == NULL)
    {
      return JS_EXCEPTION;
    }

  label = lv_obj_create(g_qpk.root);
  lv_obj_set_pos(label, qpk_arg_int(context, argc, argv, 1, 0),
                 qpk_arg_int(context, argc, argv, 2, 0));
  lv_obj_set_size(label, qpk_arg_int(context, argc, argv, 3, 64),
                  qpk_arg_int(context, argc, argv, 4, 64));
  lv_obj_set_style_bg_opa(label, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(label, 0, 0);
  lv_obj_set_style_pad_all(label, 0, 0);
  lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *number = lv_label_create(label);
  lv_label_set_text(number, text);
  lv_obj_set_style_text_color(number,
      lv_color_hex(qpk_arg_color(context, argc, argv, 5, 0xffffff)), 0);
  qpk_number_style(label, text);
  handle = qpk_add_widget(label, QPK_WIDGET_NUMBER);
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

  if (g_qpk.widget_types[handle - 1] == QPK_WIDGET_BUTTON)
    {
      lv_obj_t *label = lv_obj_get_child(g_qpk.widgets[handle - 1], 0);

      if (label != NULL)
        {
          lv_label_set_text(label, text);
        }
    }
  else if (g_qpk.widget_types[handle - 1] == QPK_WIDGET_NUMBER)
    {
      qpk_number_style(g_qpk.widgets[handle - 1], text);
    }
  else if (g_qpk.widget_types[handle - 1] == QPK_WIDGET_LABEL)
    {
      lv_label_set_text(g_qpk.widgets[handle - 1], text);
    }
  else
    {
      JS_FreeCString(context, text);
      return JS_ThrowTypeError(context, "widget does not contain text");
    }

  JS_FreeCString(context, text);
  return JS_UNDEFINED;
}

static JSValue js_ui_set_hidden(JSContext *context,
                                JSValueConst this_value,
                                int argc, JSValueConst *argv)
{
  int handle;
  int hidden;

  (void)this_value;
  handle = qpk_arg_int(context, argc, argv, 0, 0);
  hidden = qpk_arg_int(context, argc, argv, 1, 0);
  if (handle <= 0 || handle > QPK_MAX_WIDGETS ||
      g_qpk.widgets[handle - 1] == NULL)
    {
      return JS_ThrowRangeError(context, "invalid widget handle");
    }

  if (hidden)
    {
      lv_obj_add_flag(g_qpk.widgets[handle - 1], LV_OBJ_FLAG_HIDDEN);
    }
  else
    {
      lv_obj_remove_flag(g_qpk.widgets[handle - 1], LV_OBJ_FLAG_HIDDEN);
    }

  return JS_UNDEFINED;
}

static JSValue js_ui_background(JSContext *context,
                                JSValueConst this_value,
                                int argc, JSValueConst *argv)
{
  uint32_t color;

  (void)this_value;
  color = qpk_arg_color(context, argc, argv, 0, 0xffffff);
  lv_obj_set_style_bg_color(g_qpk.root, lv_color_hex(color), 0);
  lv_obj_set_style_bg_opa(g_qpk.root, LV_OPA_COVER, 0);
  return JS_UNDEFINED;
}

static JSValue js_ui_get_size(JSContext *context,
                              JSValueConst this_value,
                              int argc, JSValueConst *argv)
{
  JSValue size;

  (void)this_value;
  (void)argc;
  (void)argv;
  lv_obj_update_layout(g_qpk.root);
  size = JS_NewObject(context);
  JS_SetPropertyStr(context, size, "width",
                    JS_NewInt32(context, lv_obj_get_width(g_qpk.root)));
  JS_SetPropertyStr(context, size, "height",
                    JS_NewInt32(context, lv_obj_get_height(g_qpk.root)));
  return size;
}

static JSValue js_ui_set_color(JSContext *context,
                               JSValueConst this_value,
                               int argc, JSValueConst *argv)
{
  int handle;
  uint32_t color;
  lv_obj_t *widget;

  (void)this_value;
  handle = qpk_arg_int(context, argc, argv, 0, 0);
  color = qpk_arg_color(context, argc, argv, 1, 0xffffff);
  if (handle <= 0 || handle > QPK_MAX_WIDGETS ||
      g_qpk.widgets[handle - 1] == NULL)
    {
      return JS_ThrowRangeError(context, "invalid widget handle");
    }

  widget = g_qpk.widgets[handle - 1];
  if (g_qpk.widget_types[handle - 1] == QPK_WIDGET_LABEL ||
      g_qpk.widget_types[handle - 1] == QPK_WIDGET_NUMBER)
    {
      lv_obj_t *text = g_qpk.widget_types[handle - 1] == QPK_WIDGET_NUMBER ?
                       lv_obj_get_child(widget, 0) : widget;
      if (text != NULL)
        {
          lv_obj_set_style_text_color(text, lv_color_hex(color), 0);
        }
    }
  else if (g_qpk.widget_types[handle - 1] == QPK_WIDGET_BUTTON)
    {
      lv_obj_t *label = lv_obj_get_child(widget, 0);

      lv_obj_set_style_bg_color(widget, lv_color_hex(color), 0);
      if (label != NULL)
        {
          unsigned int brightness = ((color >> 16) & 0xff) +
                                    ((color >> 8) & 0xff) +
                                    (color & 0xff);
          lv_obj_set_style_text_color(label,
              lv_color_hex(brightness > 510 ? 0x172033 : 0xffffff), 0);
        }
    }
  else
    {
      lv_obj_set_style_bg_color(widget, lv_color_hex(color), 0);
    }

  return JS_UNDEFINED;
}

static JSValue js_ui_panel(JSContext *context, JSValueConst this_value,
                           int argc, JSValueConst *argv)
{
  lv_obj_t *panel;
  int handle;
  int opacity;

  (void)this_value;
  panel = lv_obj_create(g_qpk.root);
  if (panel == NULL)
    {
      return JS_ThrowInternalError(context, "cannot create panel");
    }

  lv_obj_set_pos(panel, qpk_arg_int(context, argc, argv, 0, 0),
                 qpk_arg_int(context, argc, argv, 1, 0));
  lv_obj_set_size(panel, qpk_arg_int(context, argc, argv, 2, 100),
                  qpk_arg_int(context, argc, argv, 3, 100));
  lv_obj_set_style_bg_color(panel,
      lv_color_hex(qpk_arg_color(context, argc, argv, 4, 0xffffff)), 0);
  opacity = qpk_arg_int(context, argc, argv, 6, LV_OPA_COVER);
  if (opacity < LV_OPA_TRANSP)
    {
      opacity = LV_OPA_TRANSP;
    }
  else if (opacity > LV_OPA_COVER)
    {
      opacity = LV_OPA_COVER;
    }

  lv_obj_set_style_bg_opa(panel, opacity, 0);
  lv_obj_set_style_border_width(panel, 0, 0);
  lv_obj_set_style_radius(panel,
                          qpk_arg_int(context, argc, argv, 5, 0), 0);
  lv_obj_set_style_pad_all(panel, 0, 0);
  lv_obj_remove_flag(panel, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  handle = qpk_add_widget(panel, QPK_WIDGET_PANEL);
  if (handle == 0)
    {
      lv_obj_delete(panel);
      return JS_ThrowInternalError(context, "too many widgets");
    }

  return JS_NewInt32(context, handle);
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
  handle = qpk_add_widget(button, QPK_WIDGET_BUTTON);
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

static bool qpk_storage_component_valid(const char *text, size_t max_len)
{
  const char *cursor;
  size_t length;

  if (text == NULL)
    {
      return false;
    }

  length = strlen(text);
  if (length == 0 || length > max_len)
    {
      return false;
    }

  for (cursor = text; *cursor != '\0'; cursor++)
    {
      if (*cursor == '/' || *cursor == '\\' || *cursor == ':' ||
          (*cursor == '.' && cursor[1] == '.'))
        {
          return false;
        }
    }

  return true;
}

static int qpk_storage_mkdir(const char *path)
{
  struct stat info;

  if (mkdir(path, 0777) == 0)
    {
      return 0;
    }

  if (errno != EEXIST || stat(path, &info) < 0 || !S_ISDIR(info.st_mode))
    {
      return -errno;
    }

  return 0;
}

static int qpk_storage_app_dir(char *path, size_t path_len)
{
  int length;

  if (!qpk_storage_component_valid(g_qpk.package,
                                   sizeof(g_qpk.package) - 1))
    {
      return -EINVAL;
    }

  length = snprintf(path, path_len, "%s/%s", QPK_STORAGE_ROOT,
                    g_qpk.package);
  return length < 0 || (size_t)length >= path_len ? -ENAMETOOLONG : 0;
}

static int qpk_storage_prepare(void)
{
  char app_dir[QPK_STORAGE_PATH_MAX];
  int ret;

  ret = qpk_storage_mkdir(CONFIG_SYSTEM_DESKTOP_QPK_DIR);
  if (ret < 0)
    {
      return ret;
    }

  ret = qpk_storage_mkdir(QPK_STORAGE_ROOT);
  if (ret < 0)
    {
      return ret;
    }

  ret = qpk_storage_app_dir(app_dir, sizeof(app_dir));
  return ret < 0 ? ret : qpk_storage_mkdir(app_dir);
}

static int qpk_storage_path(const char *key, char *path, size_t path_len)
{
  char app_dir[QPK_STORAGE_PATH_MAX];
  int length;
  int ret;

  if (!qpk_storage_component_valid(key, QPK_STORAGE_KEY_MAX))
    {
      return -EINVAL;
    }

  ret = qpk_storage_app_dir(app_dir, sizeof(app_dir));
  if (ret < 0)
    {
      return ret;
    }

  length = snprintf(path, path_len, "%s/%s.txt", app_dir, key);
  return length < 0 || (size_t)length >= path_len ? -ENAMETOOLONG : 0;
}

static int qpk_storage_write(const char *path, const char *value,
                             size_t value_len)
{
  char temporary[QPK_STORAGE_PATH_MAX + 8];
  FILE *stream;
  int length;
  int ret = 0;

  length = snprintf(temporary, sizeof(temporary), "%s.tmp", path);
  if (length < 0 || (size_t)length >= sizeof(temporary))
    {
      return -ENAMETOOLONG;
    }

  stream = fopen(temporary, "wb");
  if (stream == NULL)
    {
      return -errno;
    }

  if (fwrite(value, 1, value_len, stream) != value_len)
    {
      ret = errno == 0 ? -EIO : -errno;
    }
  else if (fflush(stream) != 0 || fsync(fileno(stream)) < 0)
    {
      ret = -errno;
    }

  if (fclose(stream) != 0 && ret == 0)
    {
      ret = -errno;
    }

  if (ret == 0 && rename(temporary, path) < 0)
    {
      ret = -errno;
    }

  if (ret < 0)
    {
      unlink(temporary);
      return ret;
    }

  sync();
  return 0;
}

static JSValue js_storage_get(JSContext *context,
                              JSValueConst this_value,
                              int argc, JSValueConst *argv)
{
  char path[QPK_STORAGE_PATH_MAX];
  uint8_t *buffer;
  const char *key;
  size_t value_len;
  JSValue result;
  int saved_errno;
  FILE *stream;
  int ret;

  (void)this_value;
  key = qpk_arg_string(context, argc, argv, 0);
  if (key == NULL)
    {
      return JS_EXCEPTION;
    }

  ret = qpk_storage_path(key, path, sizeof(path));
  JS_FreeCString(context, key);
  if (ret < 0)
    {
      return JS_ThrowRangeError(context, "invalid storage key");
    }

  buffer = malloc(QPK_STORAGE_VALUE_MAX + 1);
  if (buffer == NULL)
    {
      return JS_ThrowOutOfMemory(context);
    }

  stream = fopen(path, "rb");
  if (stream == NULL)
    {
      saved_errno = errno;
      free(buffer);
      if (saved_errno == ENOENT)
        {
          return JS_NULL;
        }

      return JS_ThrowInternalError(context, "storage unavailable: %d",
                                   saved_errno);
    }

  value_len = fread(buffer, 1, QPK_STORAGE_VALUE_MAX + 1, stream);
  saved_errno = ferror(stream) ? errno : 0;
  fclose(stream);
  if (saved_errno != 0)
    {
      free(buffer);
      return JS_ThrowInternalError(context, "storage read failed: %d",
                                   saved_errno);
    }

  if (value_len > QPK_STORAGE_VALUE_MAX)
    {
      free(buffer);
      return JS_ThrowInternalError(context, "stored value is too large");
    }

  result = JS_NewStringLen(context, (const char *)buffer, value_len);
  free(buffer);
  return result;
}

static JSValue js_storage_set(JSContext *context,
                              JSValueConst this_value,
                              int argc, JSValueConst *argv)
{
  char path[QPK_STORAGE_PATH_MAX];
  const char *key;
  const char *value;
  size_t value_len;
  int ret;

  (void)this_value;
  if (argc < 2)
    {
      return JS_ThrowTypeError(context, "storage.set requires key and value");
    }

  key = JS_ToCString(context, argv[0]);
  if (key == NULL)
    {
      return JS_EXCEPTION;
    }

  ret = qpk_storage_path(key, path, sizeof(path));
  JS_FreeCString(context, key);
  if (ret < 0)
    {
      return JS_ThrowRangeError(context, "invalid storage key");
    }

  value = JS_ToCStringLen(context, &value_len, argv[1]);
  if (value == NULL)
    {
      return JS_EXCEPTION;
    }

  if (value_len >= QPK_STORAGE_VALUE_MAX)
    {
      JS_FreeCString(context, value);
      return JS_ThrowRangeError(context, "stored value is too large");
    }

  ret = qpk_storage_prepare();
  if (ret == 0)
    {
      ret = qpk_storage_write(path, value, value_len);
    }

  JS_FreeCString(context, value);
  if (ret < 0)
    {
      return JS_ThrowInternalError(context, "storage write failed: %d",
                                   -ret);
    }

  return JS_UNDEFINED;
}

static JSValue js_storage_delete(JSContext *context,
                                 JSValueConst this_value,
                                 int argc, JSValueConst *argv)
{
  char path[QPK_STORAGE_PATH_MAX];
  const char *key;
  int saved_errno;
  int ret;

  (void)this_value;
  key = qpk_arg_string(context, argc, argv, 0);
  if (key == NULL)
    {
      return JS_EXCEPTION;
    }

  ret = qpk_storage_path(key, path, sizeof(path));
  JS_FreeCString(context, key);
  if (ret < 0)
    {
      return JS_ThrowRangeError(context, "invalid storage key");
    }

  ret = unlink(path);
  saved_errno = errno;
  if (ret < 0 && saved_errno != ENOENT)
    {
      return JS_ThrowInternalError(context, "storage delete failed: %d",
                                   saved_errno);
    }

  if (ret == 0)
    {
      sync();
    }

  return JS_UNDEFINED;
}

static void qpk_input_finish(bool submit)
{
  JSContext *context = g_qpk.context;
  JSValue callback;
  JSValue argument = JS_UNDEFINED;
  JSValue result;

  if (context == NULL || g_qpk.input_shade == NULL)
    {
      return;
    }

  callback = JS_DupValue(context, g_qpk.input_callback);
  if (submit)
    {
      argument = JS_NewString(context,
                              lv_textarea_get_text(g_qpk.input_textarea));
    }

  JS_FreeValue(context, g_qpk.input_callback);
  g_qpk.input_callback = JS_UNDEFINED;
  lv_obj_delete(g_qpk.input_shade);
  g_qpk.input_shade = NULL;
  g_qpk.input_textarea = NULL;

  if (submit)
    {
      result = qpk_call_args(callback, QPK_EVENT_BUDGET, 1, &argument);
      JS_FreeValue(context, result);
      JS_FreeValue(context, argument);
    }

  JS_FreeValue(context, callback);
}

static void qpk_input_cancel(lv_event_t *event)
{
  (void)event;
  qpk_input_finish(false);
}

static void qpk_input_submit(lv_event_t *event)
{
  (void)event;
  qpk_input_finish(true);
}

static lv_obj_t *qpk_input_button(lv_obj_t *parent, const char *text,
                                  int x, uint32_t color,
                                  lv_event_cb_t callback)
{
  lv_obj_t *button;
  lv_obj_t *label;

  button = lv_button_create(parent);
  lv_obj_set_pos(button, x, 16);
  lv_obj_set_size(button, 92, 44);
  lv_obj_set_style_bg_color(button, lv_color_hex(color), 0);
  lv_obj_set_style_radius(button, 8, 0);
  lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, NULL);
  label = lv_label_create(button);
  lv_label_set_text(label, text);
  lv_obj_set_style_text_color(label, lv_color_hex(0xffffff), 0);
  if (g_qpk.font_cb != NULL)
    {
      lv_obj_set_style_text_font(label, g_qpk.font_cb(20), 0);
    }

  lv_obj_center(label);
  return button;
}

static JSValue js_prompt_input(JSContext *context,
                               JSValueConst this_value,
                               int argc, JSValueConst *argv)
{
  JSValue title_value = JS_UNDEFINED;
  JSValue placeholder_value = JS_UNDEFINED;
  JSValue text_value = JS_UNDEFINED;
  JSValue max_value = JS_UNDEFINED;
  const char *title = "添加城市";
  const char *placeholder = "输入城市拼音，例如 jingjiang";
  const char *text = "";
  const char *converted_title = NULL;
  const char *converted_placeholder = NULL;
  const char *converted_text = NULL;
  lv_obj_t *box;
  lv_obj_t *label;
  lv_obj_t *keyboard;
  int32_t max_length = 24;
  int root_width;
  int root_height;
  int box_width;
  int box_height;

  (void)this_value;
  if (argc < 2 || !JS_IsObject(argv[0]) ||
      !JS_IsFunction(context, argv[1]))
    {
      return JS_ThrowTypeError(context,
                               "prompt.input requires options and callback");
    }

  if (g_qpk.input_shade != NULL)
    {
      return JS_ThrowInternalError(context, "an input dialog is already open");
    }

  title_value = JS_GetPropertyStr(context, argv[0], "title");
  placeholder_value = JS_GetPropertyStr(context, argv[0], "placeholder");
  text_value = JS_GetPropertyStr(context, argv[0], "value");
  max_value = JS_GetPropertyStr(context, argv[0], "maxLength");
  if (!JS_IsUndefined(title_value) && !JS_IsNull(title_value))
    {
      converted_title = JS_ToCString(context, title_value);
      if (converted_title != NULL)
        {
          title = converted_title;
        }
    }

  if (!JS_IsUndefined(placeholder_value) && !JS_IsNull(placeholder_value))
    {
      converted_placeholder = JS_ToCString(context, placeholder_value);
      if (converted_placeholder != NULL)
        {
          placeholder = converted_placeholder;
        }
    }

  if (!JS_IsUndefined(text_value) && !JS_IsNull(text_value))
    {
      converted_text = JS_ToCString(context, text_value);
      if (converted_text != NULL)
        {
          text = converted_text;
        }
    }

  if (!JS_IsUndefined(max_value))
    {
      (void)JS_ToInt32(context, &max_length, max_value);
    }

  if (max_length < 1)
    {
      max_length = 1;
    }
  else if (max_length > QPK_STORAGE_KEY_MAX)
    {
      max_length = QPK_STORAGE_KEY_MAX;
    }

  lv_obj_update_layout(g_qpk.root);
  root_width = lv_obj_get_width(g_qpk.root);
  root_height = lv_obj_get_height(g_qpk.root);
  box_width = root_width;
  box_height = root_height;

  g_qpk.input_shade = lv_obj_create(g_qpk.root);
  lv_obj_set_pos(g_qpk.input_shade, 0, 0);
  lv_obj_set_size(g_qpk.input_shade, root_width, root_height);
  lv_obj_set_style_bg_color(g_qpk.input_shade, lv_color_hex(0x02090d), 0);
  lv_obj_set_style_bg_opa(g_qpk.input_shade, LV_OPA_80, 0);
  lv_obj_set_style_border_width(g_qpk.input_shade, 0, 0);
  lv_obj_set_style_pad_all(g_qpk.input_shade, 0, 0);
  lv_obj_remove_flag(g_qpk.input_shade, LV_OBJ_FLAG_SCROLLABLE);

  box = lv_obj_create(g_qpk.input_shade);
  lv_obj_set_pos(box, 0, 0);
  lv_obj_set_size(box, box_width, box_height);
  lv_obj_set_style_bg_color(box, lv_color_hex(0x102129), 0);
  lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(box, 0, 0);
  lv_obj_set_style_radius(box, 0, 0);
  lv_obj_set_style_pad_all(box, 0, 0);
  lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

  label = lv_label_create(box);
  lv_label_set_text(label, title);
  lv_obj_set_pos(label, 24, 22);
  lv_obj_set_style_text_color(label, lv_color_hex(0xf4fbfc), 0);
  if (g_qpk.font_cb != NULL)
    {
      lv_obj_set_style_text_font(label, g_qpk.font_cb(28), 0);
    }

  qpk_input_button(box, "取消", box_width - 208, 0x40515a,
                   qpk_input_cancel);
  qpk_input_button(box, "确定", box_width - 108, 0x16758a,
                   qpk_input_submit);

  g_qpk.input_textarea = lv_textarea_create(box);
  lv_obj_set_pos(g_qpk.input_textarea, 24, 76);
  lv_obj_set_size(g_qpk.input_textarea, box_width - 48, 52);
  lv_textarea_set_one_line(g_qpk.input_textarea, true);
  lv_textarea_set_max_length(g_qpk.input_textarea, max_length);
  lv_textarea_set_placeholder_text(g_qpk.input_textarea, placeholder);
  lv_textarea_set_text(g_qpk.input_textarea, text);
  if (g_qpk.font_cb != NULL)
    {
      lv_obj_set_style_text_font(g_qpk.input_textarea,
                                 g_qpk.font_cb(20), 0);
    }

  keyboard = lv_keyboard_create(g_qpk.input_shade);
  lv_obj_set_size(keyboard, lv_pct(100), 255);
  lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);

  lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
  lv_keyboard_set_textarea(keyboard, g_qpk.input_textarea);
  lv_obj_add_event_cb(keyboard, qpk_input_submit, LV_EVENT_READY, NULL);
  lv_obj_add_event_cb(keyboard, qpk_input_cancel, LV_EVENT_CANCEL, NULL);

  g_qpk.input_callback = JS_DupValue(context, argv[1]);
  lv_obj_move_foreground(g_qpk.input_shade);

  if (converted_title != NULL)
    {
      JS_FreeCString(context, converted_title);
    }

  if (converted_placeholder != NULL)
    {
      JS_FreeCString(context, converted_placeholder);
    }

  if (converted_text != NULL)
    {
      JS_FreeCString(context, converted_text);
    }

  JS_FreeValue(context, title_value);
  JS_FreeValue(context, placeholder_value);
  JS_FreeValue(context, text_value);
  JS_FreeValue(context, max_value);
  return JS_UNDEFINED;
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
  JSValue system;

  global = JS_GetGlobalObject(context);
  object = JS_NewObject(context);
  JS_SetPropertyStr(context, object, "text",
                    JS_NewCFunction(context, js_ui_text, "text", 5));
  JS_SetPropertyStr(context, object, "number",
                    JS_NewCFunction(context, js_ui_number, "number", 6));
  JS_SetPropertyStr(context, object, "setText",
                    JS_NewCFunction(context, js_ui_set_text, "setText", 2));
  JS_SetPropertyStr(context, object, "setHidden",
                    JS_NewCFunction(context, js_ui_set_hidden,
                                    "setHidden", 2));
  JS_SetPropertyStr(context, object, "background",
                    JS_NewCFunction(context, js_ui_background,
                                    "background", 1));
  JS_SetPropertyStr(context, object, "getSize",
                    JS_NewCFunction(context, js_ui_get_size,
                                    "getSize", 0));
  JS_SetPropertyStr(context, object, "setColor",
                    JS_NewCFunction(context, js_ui_set_color,
                                    "setColor", 2));
  JS_SetPropertyStr(context, object, "panel",
                    JS_NewCFunction(context, js_ui_panel, "panel", 7));
  JS_SetPropertyStr(context, object, "button",
                    JS_NewCFunction(context, js_ui_button, "button", 7));
  JS_SetPropertyStr(context, object, "onSwipe",
                    JS_NewCFunction(context, js_ui_on_swipe,
                                    "onSwipe", 1));
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
  JS_SetPropertyStr(context, object, "input",
                    JS_NewCFunction(context, js_prompt_input,
                                    "input", 2));
  JS_SetPropertyStr(context, global, "prompt", object);

  system = JS_GetPropertyStr(context, global, "system");
  if (!JS_IsObject(system))
    {
      JS_FreeValue(context, system);
      system = JS_NewObject(context);
    }

  object = JS_NewObject(context);
  JS_SetPropertyStr(context, object, "get",
                    JS_NewCFunction(context, js_storage_get, "get", 1));
  JS_SetPropertyStr(context, object, "set",
                    JS_NewCFunction(context, js_storage_set, "set", 2));
  JS_SetPropertyStr(context, object, "delete",
                    JS_NewCFunction(context, js_storage_delete,
                                    "delete", 1));
  JS_SetPropertyStr(context, system, "storage", object);
  JS_SetPropertyStr(context, global, "system", system);

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
                       qpk_number_font_cb_t number_font_cb,
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
  g_qpk.input_callback = JS_UNDEFINED;
  g_qpk.root = root;
  g_qpk.font_cb = font_cb;
  g_qpk.number_font_cb = number_font_cb;
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
  lv_obj_remove_flag(g_qpk.root, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_event_cb(g_qpk.root, qpk_event_swiped, LV_EVENT_GESTURE, NULL);
  qpk_net_install(g_qpk.context);
  g_qpk.net_timer = lv_timer_create(qpk_net_timer_cb, QPK_NET_POLL_MS,
                                    NULL);
  if (g_qpk.net_timer == NULL)
    {
      qpk_runtime_stop();
      return -ENOMEM;
    }

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
      if (g_qpk.input_shade != NULL)
        {
          lv_obj_delete(g_qpk.input_shade);
          g_qpk.input_shade = NULL;
          g_qpk.input_textarea = NULL;
        }

      if (!JS_IsUndefined(g_qpk.input_callback))
        {
          JS_FreeValue(g_qpk.context, g_qpk.input_callback);
          g_qpk.input_callback = JS_UNDEFINED;
        }

      if (g_qpk.net_timer != NULL)
        {
          lv_timer_delete(g_qpk.net_timer);
          g_qpk.net_timer = NULL;
        }

      qpk_net_cancel(g_qpk.context);

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

      if (g_qpk.swipe_event.used)
        {
          JS_FreeValue(g_qpk.context, g_qpk.swipe_event.function);
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
