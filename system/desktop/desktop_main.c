/****************************************************************************
 * apps/system/desktop/desktop_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Chinese phone-style launcher for ESP32-P4 Function-EV-Board.
 ****************************************************************************/

#include <nuttx/config.h>

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <nuttx/sched.h>
#include <lvgl/lvgl.h>

#include "qpk_runtime.h"

#define QPK_DIR       CONFIG_SYSTEM_DESKTOP_QPK_DIR
#define MAX_QPK       8
#define NAME_MAXLEN   48
#define PANEL_WIDTH   760
#define PANEL_HEIGHT  470
#define QAPP_HEADER_HEIGHT  64

extern const uint8_t g_desktop_font_start[];
extern const uint8_t g_desktop_font_end[];

struct qpk_entry_s
{
  char dir[64];
  char name[NAME_MAXLEN];
  char package[NAME_MAXLEN];
  char version[24];
  char entry[64];
};

struct builtin_qpk_s
{
  struct qpk_entry_s manifest;
  const char *kind;
  const char *format;
};

/* Built-in QPK metadata.  The manifest name is used consistently by the
 * launcher list, the full-screen title bar, and app.getInfo().
 */

static const struct builtin_qpk_s g_builtin_qpk =
{
  .manifest =
    {
      .name = "你好快应用",
      .package = "com.example.hello",
      .version = "1.0.2",
      .entry = "builtin:/hello/app.js",
    },
  .kind = "内置示例",
  .format = "QPK 1.0",
};

struct desktop_env_s
{
  lv_obj_t *screen;
  lv_obj_t *statusbar;
  lv_obj_t *clock_label;
  lv_obj_t *home_title;
  lv_obj_t *home_hint;
  lv_obj_t *grid;
  lv_obj_t *panel;
  lv_obj_t *current_card;
  lv_obj_t *toast;
  lv_font_t *font16;
  lv_font_t *font20;
  lv_font_t *font28;
  struct qpk_entry_s qpk[MAX_QPK];
  int nqpk;
  bool light_theme;
};

static struct desktop_env_s g_desktop;

enum builtin_id_e
{
  APP_SETTINGS = 0,
  APP_QPK,
  APP_ABOUT
};

static const lv_font_t *zh_font(int size)
{
  if (size >= 28 && g_desktop.font28 != NULL)
    {
      return g_desktop.font28;
    }

  if (size >= 20 && g_desktop.font20 != NULL)
    {
      return g_desktop.font20;
    }

  if (g_desktop.font16 != NULL)
    {
      return g_desktop.font16;
    }

  return &lv_font_montserrat_24;
}

static uint32_t theme_primary(void)
{
  return g_desktop.light_theme ? 0x172033 : 0xffffff;
}

static uint32_t theme_secondary(void)
{
  return g_desktop.light_theme ? 0x65708a : 0x8f9bb5;
}

static uint32_t theme_card(void)
{
  return g_desktop.light_theme ? 0xf8f9fc : 0x151c2d;
}

static uint32_t theme_surface(void)
{
  return g_desktop.light_theme ? 0xe8ebf2 : 0x222b40;
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text,
                            uint32_t color, int size)
{
  lv_obj_t *label = lv_label_create(parent);

  lv_label_set_text(label, text);
  lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
  lv_obj_set_style_text_font(label, zh_font(size), 0);
  return label;
}

static void json_get_string(const char *json, const char *field,
                            char *out, size_t outlen)
{
  const char *p;
  const char *end;

  out[0] = '\0';
  p = strstr(json, field);
  if (p == NULL)
    {
      return;
    }

  p = strchr(p + strlen(field), ':');
  if (p == NULL)
    {
      return;
    }

  p = strchr(p, '"');
  if (p == NULL)
    {
      return;
    }

  p++;
  end = strchr(p, '"');
  if (end == NULL)
    {
      return;
    }

  if ((size_t)(end - p) >= outlen)
    {
      end = p + outlen - 1;
    }

  memcpy(out, p, end - p);
  out[end - p] = '\0';
}

static bool qpk_probe(const char *dirpath, struct qpk_entry_s *entry)
{
  char path[192];
  FILE *fp;
  char buf[1024];
  size_t n;

  if (snprintf(path, sizeof(path), "%s/manifest.json", dirpath) >=
      sizeof(path))
    {
      return false;
    }

  fp = fopen(path, "r");
  if (fp == NULL)
    {
      return false;
    }

  n = fread(buf, 1, sizeof(buf) - 1, fp);
  fclose(fp);
  buf[n] = '\0';

  json_get_string(buf, "\"name\"", entry->name, sizeof(entry->name));
  json_get_string(buf, "\"package\"", entry->package,
                  sizeof(entry->package));
  json_get_string(buf, "\"versionName\"", entry->version,
                  sizeof(entry->version));
  json_get_string(buf, "\"entry\"", entry->entry, sizeof(entry->entry));
  return entry->name[0] != '\0';
}

static void qpk_scan(void)
{
  DIR *dp;
  struct dirent *ent;
  char path[192];
  struct stat st;

  g_desktop.nqpk = 0;
  dp = opendir(QPK_DIR);
  if (dp == NULL)
    {
      return;
    }

  while ((ent = readdir(dp)) != NULL && g_desktop.nqpk < MAX_QPK)
    {
      struct qpk_entry_s *qpk = &g_desktop.qpk[g_desktop.nqpk];

      if (ent->d_name[0] == '.' ||
          snprintf(path, sizeof(path), "%s/%s", QPK_DIR, ent->d_name) >=
          sizeof(path) || stat(path, &st) != 0 || !S_ISDIR(st.st_mode))
        {
          continue;
        }

      memset(qpk, 0, sizeof(*qpk));
      if (!qpk_probe(path, qpk))
        {
          continue;
        }

      strlcpy(qpk->dir, ent->d_name, sizeof(qpk->dir));
      g_desktop.nqpk++;
    }

  closedir(dp);
}

static void apply_theme(void)
{
  uint32_t screen = g_desktop.light_theme ? 0xf3f5fa : 0x090d18;
  uint32_t bar = g_desktop.light_theme ? 0xffffff : 0x11182a;
  uint32_t tile = g_desktop.light_theme ? 0xffffff : 0x202940;
  uint32_t primary = g_desktop.light_theme ? 0x172033 : 0xffffff;
  uint32_t secondary = g_desktop.light_theme ? 0x65708a : 0x8793ad;
  int i;

  lv_obj_set_style_bg_color(g_desktop.screen, lv_color_hex(screen), 0);
  lv_obj_set_style_bg_color(g_desktop.statusbar, lv_color_hex(bar), 0);
  lv_obj_set_style_text_color(g_desktop.clock_label,
                              lv_color_hex(primary), 0);
  lv_obj_set_style_text_color(g_desktop.home_title,
                              lv_color_hex(primary), 0);
  lv_obj_set_style_text_color(g_desktop.home_hint,
                              lv_color_hex(secondary), 0);
  for (i = 0; i < lv_obj_get_child_count(g_desktop.grid); i++)
    {
      lv_obj_t *child = lv_obj_get_child(g_desktop.grid, i);
      lv_obj_set_style_bg_color(child, lv_color_hex(tile), 0);
      lv_obj_set_style_text_color(lv_obj_get_child(child, 1),
                                  lv_color_hex(primary), 0);
      lv_obj_set_style_text_color(lv_obj_get_child(child, 2),
                                  lv_color_hex(secondary), 0);
    }
}

static void panel_hide(lv_event_t *e)
{
  LV_UNUSED(e);
  qpk_runtime_stop();
  lv_obj_add_flag(g_desktop.panel, LV_OBJ_FLAG_HIDDEN);
  g_desktop.current_card = NULL;
}

static lv_obj_t *panel_card(const char *title)
{
  lv_obj_t *card;
  lv_obj_t *close;
  lv_obj_t *label;

  qpk_runtime_stop();
  lv_obj_remove_flag(g_desktop.panel, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clean(g_desktop.panel);
  lv_obj_set_style_bg_color(g_desktop.panel, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(g_desktop.panel, LV_OPA_70, 0);
  lv_obj_set_style_pad_all(g_desktop.panel, 0, 0);

  card = lv_obj_create(g_desktop.panel);
  g_desktop.current_card = card;
  lv_obj_set_size(card, PANEL_WIDTH, PANEL_HEIGHT);
  lv_obj_center(card);
  lv_obj_set_style_bg_color(card, lv_color_hex(theme_card()), 0);
  lv_obj_set_style_border_color(card,
                                lv_color_hex(g_desktop.light_theme ?
                                             0xcbd1dd : 0x35415e), 0);
  lv_obj_set_style_radius(card, 22, 0);
  lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

  label = make_label(card, title, theme_primary(), 28);
  lv_obj_align(label, LV_ALIGN_TOP_LEFT, 22, 14);

  close = lv_button_create(card);
  lv_obj_set_size(close, 54, 42);
  lv_obj_align(close, LV_ALIGN_TOP_RIGHT, -12, -4);
  lv_obj_set_style_bg_color(close, lv_color_hex(theme_surface()), 0);
  lv_obj_add_event_cb(close, panel_hide, LV_EVENT_CLICKED, NULL);
  label = lv_label_create(close);
  lv_label_set_text(label, LV_SYMBOL_CLOSE);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(label, lv_color_hex(theme_primary()), 0);
  lv_obj_center(label);
  return card;
}

static void toast_delete_cb(lv_timer_t *timer)
{
  if (g_desktop.toast != NULL)
    {
      lv_obj_delete(g_desktop.toast);
      g_desktop.toast = NULL;
    }

  lv_timer_delete(timer);
}

static void show_toast(const char *text)
{
  if (g_desktop.toast != NULL)
    {
      lv_obj_delete(g_desktop.toast);
    }

  g_desktop.toast = make_label(g_desktop.panel, text, 0xffffff, 20);
  lv_obj_set_style_bg_color(g_desktop.toast, lv_color_hex(0x30394f), 0);
  lv_obj_set_style_bg_opa(g_desktop.toast, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(g_desktop.toast, 12, 0);
  lv_obj_set_style_pad_hor(g_desktop.toast, 22, 0);
  lv_obj_set_style_pad_ver(g_desktop.toast, 12, 0);
  lv_obj_align(g_desktop.toast, LV_ALIGN_BOTTOM_MID, 0, -34);
  lv_timer_create(toast_delete_cb, 1600, NULL);
}

static const char g_hello_qpk_js[] =
  "'use strict';\n"
  "const info = app.getInfo();\n"
  "ui.text(info.packageName + '  v' + info.versionName, 24, 60, 16, ui.secondary);\n"
  "ui.text(info.name, 472, 92, 28, ui.primary);\n"
  "ui.button('Toast 提示', 362, 150, 300, 54, () => {\n"
  "  prompt.showToast({message: '来自真正 QuickJS QPK 的问候'});\n"
  "}, 0x6677f5);\n"
  "ui.button('对话框', 362, 218, 300, 54, () => {\n"
  "  prompt.dialog({title: 'QPK 运行时',\n"
  "    message: '界面由 JavaScript 创建，事件由 QuickJS 执行。'});\n"
  "}, ui.surface);\n"
  "let secs = 0;\n"
  "const tick = ui.text('已运行 0 秒', 456, 338, 16, ui.secondary);\n"
  "setInterval(() => { secs++; ui.setText(tick, '已运行 ' + secs + ' 秒'); }, 1000);\n"
  "console.log('hello QPK initialized');\n";

static void qpk_dialog_close(lv_event_t *e)
{
  lv_obj_delete(lv_event_get_user_data(e));
}

static void qpk_show_dialog(const char *text)
{
  lv_obj_t *shade;
  lv_obj_t *box;
  lv_obj_t *button;
  lv_obj_t *label;

  shade = lv_obj_create(g_desktop.panel);
  lv_obj_set_size(shade, lv_pct(100), lv_pct(100));
  lv_obj_set_style_bg_color(shade, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(shade, LV_OPA_70, 0);
  lv_obj_set_style_border_width(shade, 0, 0);
  box = lv_obj_create(shade);
  lv_obj_set_size(box, 500, 240);
  lv_obj_center(box);
  lv_obj_set_style_bg_color(box, lv_color_hex(theme_card()), 0);
  lv_obj_set_style_radius(box, 18, 0);
  label = make_label(box, text ? text : "快应用", theme_primary(), 20);
  lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(label, 440);
  lv_obj_align(label, LV_ALIGN_TOP_LEFT, 12, 12);
  button = lv_button_create(box);
  lv_obj_set_size(button, 96, 42);
  lv_obj_align(button, LV_ALIGN_BOTTOM_RIGHT, -8, -8);
  lv_obj_add_event_cb(button, qpk_dialog_close, LV_EVENT_CLICKED, shade);
  label = make_label(button, "确定", 0xffffff, 20);
  lv_obj_center(label);
}

static char *qpk_load_entry(const struct qpk_entry_s *qpk,
                            char *filename, size_t filename_size,
                            size_t *source_size)
{
  static const char *patterns[] = {"%s/%s/%s", "%s/%s/%s.js",
                                   "%s/%s/%s/index.js"};
  FILE *file;
  char *source;
  long size;
  size_t got;
  int i;

  for (i = 0; i < 3; i++)
    {
      if (snprintf(filename, filename_size, patterns[i], QPK_DIR, qpk->dir,
                   qpk->entry[0] ? qpk->entry : "app.js") >= filename_size)
        {
          continue;
        }

      file = fopen(filename, "rb");
      if (file == NULL)
        {
          continue;
        }

      if (fseek(file, 0, SEEK_END) < 0 || (size = ftell(file)) <= 0 ||
          size > 128 * 1024 || fseek(file, 0, SEEK_SET) < 0)
        {
          fclose(file);
          continue;
        }

      source = malloc((size_t)size + 1);
      if (source == NULL)
        {
          fclose(file);
          return NULL;
        }

      got = fread(source, 1, (size_t)size, file);
      fclose(file);
      if (got != (size_t)size)
        {
          free(source);
          continue;
        }

      source[got] = '\0';
      *source_size = got;
      return source;
    }

  return NULL;
}

static void qpk_clicked(lv_event_t *e);

static void qapp_back(lv_event_t *e)
{
  LV_UNUSED(e);
  qpk_runtime_stop();
  qpk_clicked(NULL);
}

static lv_obj_t *qapp_page(const char *title)
{
  lv_obj_t *page;
  lv_obj_t *content;
  lv_obj_t *back;
  lv_obj_t *label;
  int height;

  qpk_runtime_stop();
  lv_obj_remove_flag(g_desktop.panel, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clean(g_desktop.panel);
  lv_obj_set_style_bg_color(g_desktop.panel,
                            lv_color_hex(theme_card()), 0);
  lv_obj_set_style_bg_opa(g_desktop.panel, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(g_desktop.panel, 0, 0);

  page = lv_obj_create(g_desktop.panel);
  g_desktop.current_card = page;
  lv_obj_set_size(page, lv_pct(100), lv_pct(100));
  lv_obj_set_style_bg_color(page, lv_color_hex(theme_card()), 0);
  lv_obj_set_style_border_width(page, 0, 0);
  lv_obj_set_style_radius(page, 0, 0);
  lv_obj_set_style_pad_all(page, 0, 0);
  lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);

  back = lv_button_create(page);
  lv_obj_set_size(back, 52, 42);
  lv_obj_set_pos(back, 12, 10);
  lv_obj_set_style_bg_color(back, lv_color_hex(theme_surface()), 0);
  lv_obj_set_style_radius(back, 12, 0);
  lv_obj_add_event_cb(back, qapp_back, LV_EVENT_CLICKED, NULL);
  label = lv_label_create(back);
  lv_label_set_text(label, LV_SYMBOL_LEFT);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(label, lv_color_hex(theme_primary()), 0);
  lv_obj_center(label);

  label = make_label(page, title, theme_primary(), 28);
  lv_obj_set_pos(label, 78, 15);

  content = lv_obj_create(page);
  lv_obj_update_layout(page);
  height = lv_obj_get_height(page) - QAPP_HEADER_HEIGHT;
  lv_obj_set_size(content, lv_pct(100), height);
  lv_obj_align(content, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(content, 0, 0);
  lv_obj_set_style_radius(content, 0, 0);
  lv_obj_set_style_pad_all(content, 0, 0);
  lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);

  return content;
}

static void launch_builtin_qapp(lv_event_t *e)
{
  lv_obj_t *card;
  const struct qpk_entry_s *manifest = &g_builtin_qpk.manifest;

  LV_UNUSED(e);
  card = qapp_page(manifest->name);
  qpk_runtime_launch(card, manifest->name, manifest->package,
                     manifest->version, manifest->entry, g_hello_qpk_js,
                     sizeof(g_hello_qpk_js) - 1, zh_font, show_toast,
                     qpk_show_dialog);
}

static void external_qapp_clicked(lv_event_t *e)
{
  intptr_t index = (intptr_t)lv_event_get_user_data(e);
  struct qpk_entry_s *qpk;
  lv_obj_t *card;
  lv_obj_t *label;
  char *source;
  char filename[256];
  char text[256];
  size_t source_size = 0;

  if (index < 0 || index >= g_desktop.nqpk)
    {
      return;
    }

  qpk = &g_desktop.qpk[index];
  card = qapp_page(qpk->name);
  source = qpk_load_entry(qpk, filename, sizeof(filename), &source_size);
  if (source != NULL)
    {
      int ret = qpk_runtime_launch(card, qpk->name, qpk->package,
                                   qpk->version, filename, source,
                                   source_size, zh_font, show_toast,
                                   qpk_show_dialog);
      free(source);
      if (ret == 0)
        {
          return;
        }
    }

  snprintf(text, sizeof(text),
           "包名：%s\n版本：%s\n入口：%s\n目录：%s/%s\n\n"
           "无法加载 JavaScript 入口。当前运行时支持 .js QPK；"
           "MicroReactor 风格 .ux 解析器将在下一阶段接入。",
           qpk->package[0] ? qpk->package : "未声明",
           qpk->version[0] ? qpk->version : "未声明",
           qpk->entry[0] ? qpk->entry : "未声明", QPK_DIR, qpk->dir);
  label = make_label(card, text, theme_secondary(), 20);
  lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(label, 690);
  lv_obj_align(label, LV_ALIGN_TOP_LEFT, 24, 76);
}

static lv_obj_t *list_button(lv_obj_t *parent, const char *title,
                             const char *subtitle, int y,
                             lv_event_cb_t cb, void *user)
{
  lv_obj_t *button = lv_button_create(parent);
  lv_obj_t *label;

  lv_obj_set_size(button, 690, 72);
  lv_obj_align(button, LV_ALIGN_TOP_MID, 0, y);
  lv_obj_set_style_bg_color(button, lv_color_hex(theme_surface()), 0);
  lv_obj_set_style_radius(button, 14, 0);
  lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, user);
  label = make_label(button, title, theme_primary(), 20);
  lv_obj_align(label, LV_ALIGN_TOP_LEFT, 12, 2);
  label = make_label(button, subtitle, theme_secondary(), 16);
  lv_obj_align(label, LV_ALIGN_BOTTOM_LEFT, 12, -2);
  return button;
}

static void qpk_clicked(lv_event_t *e)
{
  lv_obj_t *card;
  char builtin_subtitle[96];
  int i;

  LV_UNUSED(e);
  qpk_scan();
  card = panel_card("快应用");
  snprintf(builtin_subtitle, sizeof(builtin_subtitle), "%s · %s",
           g_builtin_qpk.kind, g_builtin_qpk.format);
  list_button(card, g_builtin_qpk.manifest.name, builtin_subtitle, 72,
              launch_builtin_qapp, NULL);

  for (i = 0; i < g_desktop.nqpk && i < 4; i++)
    {
      char subtitle[96];

      snprintf(subtitle, sizeof(subtitle), "%s · %s",
               g_desktop.qpk[i].package[0] ? g_desktop.qpk[i].package :
               g_desktop.qpk[i].dir,
               g_desktop.qpk[i].version[0] ? g_desktop.qpk[i].version :
               "QPK");
      list_button(card, g_desktop.qpk[i].name, subtitle, 154 + i * 80,
                  external_qapp_clicked, (void *)(intptr_t)i);
    }

  if (g_desktop.nqpk == 0)
    {
      lv_obj_t *label = make_label(card,
                                   "外部应用目录为空：" QPK_DIR,
                                   theme_secondary(), 16);
      lv_obj_align(label, LV_ALIGN_BOTTOM_LEFT, 24, -12);
    }
}

static void settings_clicked(lv_event_t *e);

static void settings_rebuild_async(void *data)
{
  LV_UNUSED(data);
  settings_clicked(NULL);
}

static void theme_changed(lv_event_t *e)
{
  lv_obj_t *sw = lv_event_get_target(e);

  g_desktop.light_theme = lv_obj_has_state(sw, LV_STATE_CHECKED);
  apply_theme();
  lv_async_call(settings_rebuild_async, NULL);
}

static void setting_row(lv_obj_t *parent, const char *title,
                        const char *subtitle, int y, bool checked,
                        lv_event_cb_t cb)
{
  lv_obj_t *label;
  lv_obj_t *sw;

  label = make_label(parent, title, theme_primary(), 20);
  lv_obj_set_pos(label, 28, y);
  label = make_label(parent, subtitle, theme_secondary(), 16);
  lv_obj_set_pos(label, 28, y + 30);
  sw = lv_switch_create(parent);
  lv_obj_set_size(sw, 58, 32);
  lv_obj_set_pos(sw, 650, y + 8);
  if (checked)
    {
      lv_obj_add_state(sw, LV_STATE_CHECKED);
    }

  lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);
}

static void settings_clicked(lv_event_t *e)
{
  lv_obj_t *card;
  lv_obj_t *label;
  char info[256];

  LV_UNUSED(e);
  card = panel_card("设置");
  setting_row(card, "浅色桌面", "切换桌面背景与应用卡片", 82,
              g_desktop.light_theme, theme_changed);

  snprintf(info, sizeof(info),
           "设备信息\nESP32-P4 Function-EV-Board\n"
           "双核 RISC-V · PSRAM 已启用 · 触摸已连接\n"
           "NuttX 桌面 · 已发现 %d 个外部 QPK",
           g_desktop.nqpk);
  label = make_label(card, info, theme_secondary(), 16);
  lv_obj_set_pos(label, 28, 190);
}

static void about_clicked(lv_event_t *e)
{
  struct timespec ts;
  lv_obj_t *card;
  lv_obj_t *label;
  char text[320];

  LV_UNUSED(e);
  clock_gettime(CLOCK_MONOTONIC, &ts);
  card = panel_card("关于");
  snprintf(text, sizeof(text),
           "ESP32-P4 中文触摸桌面\n\n"
           "作者：电子科技大学 闻家贤\n\n"
           "系统：Apache NuttX RTOS\n"
           "处理器：双核 RISC-V\n"
           "内存：内部 RAM + PSRAM\n"
           "运行时间：%lu 秒\n"
           "中文字体：阿里巴巴普惠体 3.0 55 Regular",
           (unsigned long)ts.tv_sec);
  label = make_label(card, text, theme_secondary(), 20);
  lv_obj_set_pos(label, 24, 82);
}

static void tile_clicked(lv_event_t *e)
{
  switch ((intptr_t)lv_event_get_user_data(e))
    {
      case APP_SETTINGS:
        settings_clicked(e);
        break;
      case APP_QPK:
        qpk_clicked(e);
        break;
      case APP_ABOUT:
        about_clicked(e);
        break;
      default:
        break;
    }
}

static void add_tile(lv_obj_t *parent, const char *icon, const char *name,
                     const char *hint, uint32_t accent, intptr_t id)
{
  lv_obj_t *button;
  lv_obj_t *label;

  button = lv_button_create(parent);
  lv_obj_set_size(button, 210, 190);
  lv_obj_set_style_radius(button, 24, 0);
  lv_obj_set_style_bg_color(button, lv_color_hex(0x202940), 0);
  lv_obj_set_style_shadow_width(button, 0, 0);
  lv_obj_add_event_cb(button, tile_clicked, LV_EVENT_CLICKED, (void *)id);

  label = lv_label_create(button);
  lv_label_set_text(label, icon);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_48, 0);
  lv_obj_set_style_text_color(label, lv_color_hex(accent), 0);
  lv_obj_align(label, LV_ALIGN_TOP_LEFT, 10, 10);

  label = make_label(button, name, 0xffffff, 28);
  lv_obj_align(label, LV_ALIGN_BOTTOM_LEFT, 10, -38);
  label = make_label(button, hint, 0x8793ad, 16);
  lv_obj_align(label, LV_ALIGN_BOTTOM_LEFT, 10, -8);
}

static void clock_timer_cb(lv_timer_t *timer)
{
  struct timespec ts;
  char buf[20];

  LV_UNUSED(timer);
  clock_gettime(CLOCK_MONOTONIC, &ts);
  snprintf(buf, sizeof(buf), "%02ld:%02ld",
           (long)(ts.tv_sec / 60) % 100, (long)ts.tv_sec % 60);
  lv_label_set_text(g_desktop.clock_label, buf);
}

static lv_obj_t *statusbar_create(lv_obj_t *parent)
{
  lv_obj_t *bar = lv_obj_create(parent);
  lv_obj_t *label;

  lv_obj_set_size(bar, lv_pct(100), 44);
  lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_bg_color(bar, lv_color_hex(0x11182a), 0);
  lv_obj_set_style_border_width(bar, 0, 0);
  lv_obj_set_style_radius(bar, 0, 0);
  lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

  label = make_label(bar, "NuttX 桌面", 0x91a2ff, 20);
  lv_obj_align(label, LV_ALIGN_LEFT_MID, 14, 0);
  label = lv_label_create(bar);
  lv_label_set_text(label, LV_SYMBOL_WIFI "  " LV_SYMBOL_CHARGE);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(label, lv_color_hex(0x9aa7cc), 0);
  lv_obj_align(label, LV_ALIGN_RIGHT_MID, -104, 0);

  g_desktop.clock_label = make_label(bar, "00:00", 0xffffff, 20);
  lv_obj_align(g_desktop.clock_label, LV_ALIGN_RIGHT_MID, -12, 0);
  return bar;
}

static void desktop_ui_create(void)
{
  lv_obj_t *screen = lv_screen_active();
  size_t font_size = (size_t)(g_desktop_font_end - g_desktop_font_start);

  memset(&g_desktop, 0, sizeof(g_desktop));
  g_desktop.font16 = lv_tiny_ttf_create_data(g_desktop_font_start,
                                              font_size, 16);
  g_desktop.font20 = lv_tiny_ttf_create_data(g_desktop_font_start,
                                              font_size, 20);
  g_desktop.font28 = lv_tiny_ttf_create_data(g_desktop_font_start,
                                              font_size, 28);
  g_desktop.screen = screen;
  lv_obj_set_style_bg_color(screen, lv_color_hex(0x090d18), 0);
  g_desktop.statusbar = statusbar_create(screen);

  g_desktop.home_title = make_label(screen, "欢迎使用 ESP32-P4",
                                     0xffffff, 28);
  lv_obj_set_pos(g_desktop.home_title, 36, 72);
  g_desktop.home_hint = make_label(screen, "触摸图标打开应用",
                                    0x74819d, 16);
  lv_obj_set_pos(g_desktop.home_hint, 38, 114);

  g_desktop.grid = lv_obj_create(screen);
  lv_obj_set_size(g_desktop.grid, lv_pct(100), 230);
  lv_obj_align(g_desktop.grid, LV_ALIGN_TOP_MID, 0, 154);
  lv_obj_set_style_bg_opa(g_desktop.grid, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(g_desktop.grid, 0, 0);
  lv_obj_set_flex_flow(g_desktop.grid, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(g_desktop.grid, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(g_desktop.grid, 28, 0);

  add_tile(g_desktop.grid, LV_SYMBOL_SETTINGS, "设置", "主题与设备信息",
           0x92a1ff, APP_SETTINGS);
  add_tile(g_desktop.grid, LV_SYMBOL_FILE, "快应用", "QPK 应用与示例",
           0x55d6a7, APP_QPK);
  add_tile(g_desktop.grid, LV_SYMBOL_BELL, "关于", "系统运行状态",
           0xf2b56b, APP_ABOUT);

  qpk_scan();

  g_desktop.panel = lv_obj_create(screen);
  lv_obj_set_size(g_desktop.panel, lv_pct(100), lv_pct(100));
  lv_obj_set_style_bg_color(g_desktop.panel, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(g_desktop.panel, LV_OPA_70, 0);
  lv_obj_set_style_border_width(g_desktop.panel, 0, 0);
  lv_obj_add_flag(g_desktop.panel, LV_OBJ_FLAG_HIDDEN);

  lv_timer_create(clock_timer_cb, 1000, NULL);
  clock_timer_cb(NULL);
}

int main(int argc, FAR char *argv[])
{
  lv_nuttx_dsc_t info;
  lv_nuttx_result_t result;
#ifdef CONFIG_SYSTEM_NSH
  extern int nsh_main(int argc, FAR char *argv[]);
#endif
#ifdef CONFIG_ESP32P4_FUNCTION_EV_BOARD_TOUCHSCREEN
  extern int board_touch_initialize(void);
#endif

  LV_UNUSED(argc);
  LV_UNUSED(argv);
  if (lv_is_initialized())
    {
      return -1;
    }

#ifdef CONFIG_ESP32P4_FUNCTION_EV_BOARD_TOUCHSCREEN
  if (board_touch_initialize() < 0)
    {
      fprintf(stderr, "desktop: touch init failed\n");
    }
#endif

  lv_init();
  lv_nuttx_dsc_init(&info);
  info.fb_path = "/dev/fb0";
#ifdef CONFIG_ESP32P4_FUNCTION_EV_BOARD_TOUCHSCREEN
  info.input_path = "/dev/input0";
#else
  info.input_path = NULL;
#endif

  lv_nuttx_init(&info, &result);
  if (result.disp == NULL)
    {
      fprintf(stderr, "desktop: display init failed\n");
      return 1;
    }

  desktop_ui_create();
  lv_refr_now(result.disp);

#ifdef CONFIG_SYSTEM_NSH
  if (task_create("nsh", 100, 4096, nsh_main, NULL) < 0)
    {
      fprintf(stderr, "desktop: failed to start NSH\n");
    }
#endif

  while (1)
    {
      uint32_t idle = lv_timer_handler();
      usleep((idle ? idle : 1) * 1000);
    }

  return 0;
}
