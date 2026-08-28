/****************************************************************************
 * apps/system/desktop/desktop_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Chinese phone-style launcher for ESP32-P4 Function-EV-Board.
 ****************************************************************************/

#include <nuttx/config.h>

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <nuttx/sched.h>
#include <lvgl/lvgl.h>

#if defined(CONFIG_NET) && defined(CONFIG_NETUTILS_DHCPC) && \
    defined(CONFIG_SYSTEM_C6PROBE)
#  include <netutils/netlib.h>
#  include "c6net.h"
#  define DESKTOP_WIFI_ENABLED 1
#else
#  define DESKTOP_WIFI_ENABLED 0
#endif

#include "qpk_runtime.h"
#include "qpk_net.h"
#include "desktop_filemgr.h"
#include "desktop_camera.h"

#define QPK_DIR       CONFIG_SYSTEM_DESKTOP_QPK_DIR
#define MAX_QPK       8
#define NAME_MAXLEN   48
#define PANEL_WIDTH   760
#define PANEL_HEIGHT  470
#define QAPP_HEADER_HEIGHT  64
#define WIFI_SSID_MAXLEN    32
#define WIFI_PASSWORD_MAXLEN 64
#define WIFI_ASSOCIATE_MS   20000
#define WIFI_WORKER_PRIORITY 100
#define WIFI_WORKER_STACK   6144

extern const uint8_t g_desktop_font_start[];
extern const uint8_t g_desktop_font_end[];
extern const uint8_t g_desktop_number_font_start[];
extern const uint8_t g_desktop_number_font_end[];

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
      .name = "天气",
      .package = "com.example.weather",
      .version = "1.0.0",
      .entry = "builtin:/weather/index.js",
    },
  .kind = "网络快应用",
  .format = "QPK 1.0",
};

static const struct builtin_qpk_s g_builtin_2048_qpk =
{
  .manifest =
    {
      .name = "2048",
      .package = "com.example.game2048",
      .version = "1.0.0",
      .entry = "builtin:/2048/index.js",
    },
  .kind = "休闲游戏",
  .format = "QPK 1.0",
};

struct desktop_env_s
{
  lv_obj_t *screen;
  lv_obj_t *statusbar;
  lv_obj_t *clock_label;
  lv_obj_t *wifi_icon;
  lv_obj_t *home_title;
  lv_obj_t *home_hint;
  lv_obj_t *grid;
  lv_obj_t *panel;
  lv_obj_t *current_card;
  lv_obj_t *toast;
  lv_obj_t *wifi_status_label;
  lv_obj_t *wifi_modal;
  lv_obj_t *wifi_ssid_input;
  lv_obj_t *wifi_password_input;
  lv_obj_t *wifi_keyboard;
  lv_font_t *font16;
  lv_font_t *font20;
  lv_font_t *font28;
  lv_font_t *font48;
  lv_font_t *number_font32;
  lv_font_t *number_font40;
  lv_font_t *number_font48;
  struct qpk_entry_s qpk[MAX_QPK];
  int nqpk;
  bool light_theme;
};

static struct desktop_env_s g_desktop;
static char g_qpk_delete_dir[64];
static char g_qpk_delete_name[NAME_MAXLEN];

enum wifi_state_e
{
  WIFI_STATE_IDLE = 0,
  WIFI_STATE_STARTING,
  WIFI_STATE_ASSOCIATING,
  WIFI_STATE_DHCP,
  WIFI_STATE_CONNECTED,
  WIFI_STATE_FAILED
};

struct wifi_manager_s
{
  pthread_mutex_t lock;
  enum wifi_state_e state;
  bool busy;
  int result;
  uint32_t generation;
  char request_ssid[WIFI_SSID_MAXLEN + 1];
  char request_password[WIFI_PASSWORD_MAXLEN + 1];
  char active_ssid[WIFI_SSID_MAXLEN + 1];
  char ip[INET_ADDRSTRLEN];
};

struct wifi_snapshot_s
{
  enum wifi_state_e state;
  bool busy;
  int result;
  char ssid[WIFI_SSID_MAXLEN + 1];
  char ip[INET_ADDRSTRLEN];
};

static struct wifi_manager_s g_wifi =
{
  .lock = PTHREAD_MUTEX_INITIALIZER,
  .state = WIFI_STATE_IDLE,
};

enum builtin_id_e
{
  APP_SETTINGS = 0,
  APP_QPK,
  APP_ABOUT
};

static const lv_font_t *zh_font(int size)
{
  if (size >= 40 && g_desktop.font48 != NULL)
    {
      return g_desktop.font48;
    }

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

static const lv_font_t *number_font(int digits)
{
  if (digits <= 2 && g_desktop.number_font48 != NULL)
    {
      return g_desktop.number_font48;
    }

  if (digits == 3 && g_desktop.number_font40 != NULL)
    {
      return g_desktop.number_font40;
    }

  if (g_desktop.number_font32 != NULL)
    {
      return g_desktop.number_font32;
    }

  return digits <= 2 ? &lv_font_montserrat_48 : &lv_font_montserrat_24;
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

static void wifi_get_snapshot(FAR struct wifi_snapshot_s *snapshot)
{
  pthread_mutex_lock(&g_wifi.lock);
  snapshot->state = g_wifi.state;
  snapshot->busy = g_wifi.busy;
  snapshot->result = g_wifi.result;
  strlcpy(snapshot->ssid, g_wifi.active_ssid, sizeof(snapshot->ssid));
  strlcpy(snapshot->ip, g_wifi.ip, sizeof(snapshot->ip));
  pthread_mutex_unlock(&g_wifi.lock);
}

static bool wifi_set_progress(uint32_t generation,
                              enum wifi_state_e state)
{
  bool current;

  pthread_mutex_lock(&g_wifi.lock);
  current = g_wifi.generation == generation;
  if (current)
    {
      g_wifi.state = state;
      g_wifi.result = 0;
    }

  pthread_mutex_unlock(&g_wifi.lock);
  return current;
}

static bool wifi_finish(uint32_t generation, enum wifi_state_e state,
                        int result, FAR const char *ip)
{
  bool current;

  pthread_mutex_lock(&g_wifi.lock);
  current = g_wifi.generation == generation;
  if (current)
    {
      g_wifi.state = state;
      g_wifi.result = result;
      g_wifi.busy = false;
      if (ip != NULL)
        {
          strlcpy(g_wifi.ip, ip, sizeof(g_wifi.ip));
        }
    }

  pthread_mutex_unlock(&g_wifi.lock);
  return current;
}

#if DESKTOP_WIFI_ENABLED
static int wifi_connect_worker(int argc, FAR char *argv[])
{
  struct in_addr addr;
  char ssid[WIFI_SSID_MAXLEN + 1];
  char password[WIFI_PASSWORD_MAXLEN + 1];
  char ip[INET_ADDRSTRLEN];
  uint32_t generation;
  int elapsed;
  int ret;

  LV_UNUSED(argc);
  LV_UNUSED(argv);

  for (;;)
    {
      pthread_mutex_lock(&g_wifi.lock);
      generation = g_wifi.generation;
      strlcpy(ssid, g_wifi.request_ssid, sizeof(ssid));
      strlcpy(password, g_wifi.request_password, sizeof(password));
      memset(g_wifi.request_password, 0, sizeof(g_wifi.request_password));
      pthread_mutex_unlock(&g_wifi.lock);

      if (!wifi_set_progress(generation, WIFI_STATE_STARTING))
        {
          memset(password, 0, sizeof(password));
          continue;
        }

      ret = c6net_connect(ssid, password);
      memset(password, 0, sizeof(password));
      if (ret < 0)
        {
          if (wifi_finish(generation, WIFI_STATE_FAILED, ret, NULL))
            {
              return ret;
            }

          continue;
        }

      if (!wifi_set_progress(generation, WIFI_STATE_ASSOCIATING))
        {
          continue;
        }

      for (elapsed = 0; elapsed < WIFI_ASSOCIATE_MS; elapsed += 100)
        {
          if (!wifi_set_progress(generation, WIFI_STATE_ASSOCIATING))
            {
              break;
            }

          if (c6net_is_associated())
            {
              break;
            }

          usleep(100000);
        }

      if (!wifi_set_progress(generation, WIFI_STATE_ASSOCIATING))
        {
          continue;
        }

      if (!c6net_is_associated())
        {
          if (wifi_finish(generation, WIFI_STATE_FAILED,
                          -ETIMEDOUT, NULL))
            {
              return -ETIMEDOUT;
            }

          continue;
        }

      if (!wifi_set_progress(generation, WIFI_STATE_DHCP))
        {
          continue;
        }

      ret = netlib_obtain_ipv4addr("eth0");
      if (ret < 0)
        {
          ret = errno != 0 ? -errno : -EIO;
          if (wifi_finish(generation, WIFI_STATE_FAILED, ret, NULL))
            {
              return ret;
            }

          continue;
        }

      ip[0] = '\0';
      if (netlib_get_ipv4addr("eth0", &addr) == 0)
        {
          (void)inet_ntop(AF_INET, &addr, ip, sizeof(ip));
        }

      /* NTP and CA setup begin as soon as DHCP has installed IP, route and
       * DNS state, instead of charging that cold-start cost to the first
       * weather HTTPS request. */

      (void)qpk_net_prepare();

      if (wifi_finish(generation, WIFI_STATE_CONNECTED, 0, ip))
        {
          return 0;
        }
    }
}
#endif

static int wifi_start_connect(FAR const char *ssid,
                              FAR const char *password)
{
#if DESKTOP_WIFI_ENABLED
  pid_t pid;
  uint32_t generation;
  bool spawn;
  int ret = 0;

  if (ssid == NULL || password == NULL ||
      strlen(ssid) > WIFI_SSID_MAXLEN ||
      strlen(password) > WIFI_PASSWORD_MAXLEN)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&g_wifi.lock);
  spawn = !g_wifi.busy;
  g_wifi.busy = true;
  g_wifi.state = WIFI_STATE_STARTING;
  g_wifi.result = 0;
  g_wifi.ip[0] = '\0';
  g_wifi.generation++;
  generation = g_wifi.generation;
  strlcpy(g_wifi.request_ssid, ssid, sizeof(g_wifi.request_ssid));
  memset(g_wifi.request_password, 0, sizeof(g_wifi.request_password));
  strlcpy(g_wifi.request_password, password,
          sizeof(g_wifi.request_password));
  strlcpy(g_wifi.active_ssid, ssid, sizeof(g_wifi.active_ssid));

  pthread_mutex_unlock(&g_wifi.lock);
  if (!spawn)
    {
      return 0;
    }

  pid = task_create("wifi_connect", WIFI_WORKER_PRIORITY,
                    WIFI_WORKER_STACK, wifi_connect_worker, NULL);
  if (pid < 0)
    {
      ret = errno != 0 ? -errno : -EIO;
      pthread_mutex_lock(&g_wifi.lock);
      memset(g_wifi.request_password, 0, sizeof(g_wifi.request_password));
      pthread_mutex_unlock(&g_wifi.lock);
      (void)wifi_finish(generation, WIFI_STATE_FAILED, ret, NULL);
      return ret;
    }

  return 0;
#else
  LV_UNUSED(ssid);
  LV_UNUSED(password);
  return -ENOSYS;
#endif
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

static void wifi_modal_forget(void)
{
  g_desktop.wifi_modal = NULL;
  g_desktop.wifi_ssid_input = NULL;
  g_desktop.wifi_password_input = NULL;
  g_desktop.wifi_keyboard = NULL;
}

static void panel_content_forget(void)
{
  desktop_camera_stop();
  g_desktop.wifi_status_label = NULL;
  wifi_modal_forget();
  g_desktop.toast = NULL;
}

static void panel_hide(lv_event_t *e)
{
  LV_UNUSED(e);
  qpk_runtime_stop();
  panel_content_forget();
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
  panel_content_forget();
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
  lv_obj_t *toast = lv_timer_get_user_data(timer);

  if (g_desktop.toast == toast)
    {
      lv_obj_delete(toast);
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
  lv_timer_create(toast_delete_cb, 1600, g_desktop.toast);
}

static const char g_hello_qpk_js[] =
  "'use strict';\n"
  "import fetch from '@system.fetch';\n"
  "import network from '@system.network';\n"
  "import storage from '@system.storage';\n"
  "let cities = [{q:'jingjiang',n:'靖江'}];\n"
  "let cityIndex = 0;\n"
  "try {\n"
  "  const raw = storage.get('cities');\n"
  "  const saved = raw ? JSON.parse(raw) : null;\n"
  "  if (Array.isArray(saved) && saved.length) {\n"
  "    const valid = [];\n"
  "    for (let i = 0; i < saved.length && valid.length < 8; i++) {\n"
  "      const c = saved[i];\n"
  "      if (c && typeof c.q === 'string' && c.q && typeof c.n === 'string' && c.n) valid.push({q:c.q,n:c.n});\n"
  "    }\n"
  "    if (valid.length) cities = valid;\n"
  "  }\n"
  "  const selected = storage.get('cur');\n"
  "  for (let i = 0; selected && i < cities.length; i++) if (cities[i].q === selected) cityIndex = i;\n"
  "} catch (e) { console.log('weather storage load failed', e); }\n"
  "let loading = false;\n"
  "let hasWeather = false;\n"
  "let requestId = 0;\n"
  "const viewport = ui.getSize();\n"
  "const W = viewport.width, H = viewport.height;\n"
  "const pad = 28, gap = 12;\n"
  "const heroH = Math.floor(H * 0.38);\n"
  "const metricsY = heroH + 16;\n"
  "const metricsH = Math.floor(H * 0.17);\n"
  "const forecastY = metricsY + metricsH + 16;\n"
  "const forecastH = H - forecastY - 18;\n"
  "const metricW = Math.floor((W - pad * 2 - gap * 2) / 3);\n"
  "const colW = Math.floor((W - pad * 2 - 32) / 5);\n"
  "const tempX = Math.floor(W * 0.40);\n"
  "const actionX = W - 182;\n"
  "ui.background(0x081218);\n"
  "const hero = ui.panel(0, 0, W, heroH, 0x123e4a, 0, 255);\n"
  "ui.panel(pad, metricsY, metricW, metricsH, 0x14262e, 8, 255);\n"
  "ui.panel(pad + metricW + gap, metricsY, metricW, metricsH, 0x14262e, 8, 255);\n"
  "ui.panel(pad + (metricW + gap) * 2, metricsY, metricW, metricsH, 0x14262e, 8, 255);\n"
  "ui.panel(pad, forecastY, W - pad * 2, forecastH, 0x102129, 8, 255);\n"
  "for (let i = 1; i < 5; i++) ui.panel(pad + 16 + colW * i, forecastY + 48, 1, forecastH - 64, 0x29404a, 0, 255);\n"
  "const current = ui.text(cities[cityIndex].n, 32, 20, 28, 0xf4fbfc);\n"
  "const place = ui.text('江苏 · 泰州', 34, 58, 16, 0xb9d5da);\n"
  "const state = ui.text('正在同步时间和天气数据', 34, heroH - 38, 16, 0x9fc4ca);\n"
  "const temp = ui.text('--', tempX, 22, 48, 0xffffff);\n"
  "ui.text('度', tempX + 84, 49, 20, 0xffffff);\n"
  "const weather = ui.text('等待更新', tempX + 2, 86, 28, 0xffffff);\n"
  "const range = ui.text('最高 --度  最低 --度', tempX + 3, 130, 16, 0xc9e3e7);\n"
  "ui.text('湿度', pad + 18, metricsY + 14, 16, 0x8fa5af);\n"
  "const humidity = ui.text('--', pad + 18, metricsY + 47, 20, 0x5bc0be);\n"
  "ui.text('风向', pad + metricW + gap + 18, metricsY + 14, 16, 0x8fa5af);\n"
  "const wind = ui.text('--', pad + metricW + gap + 18, metricsY + 47, 20, 0x7bd389);\n"
  "ui.text('风力', pad + (metricW + gap) * 2 + 18, metricsY + 14, 16, 0x8fa5af);\n"
  "const power = ui.text('--', pad + (metricW + gap) * 2 + 18, metricsY + 47, 20, 0xf4b860);\n"
  "ui.text('未来五日', pad + 16, forecastY + 14, 16, 0xdce9ed);\n"
  "const dayName = [], dayWeather = [], dayRange = [];\n"
  "for (let i = 0; i < 5; i++) {\n"
  "  const x = pad + 24 + i * colW;\n"
  "  dayName.push(ui.text(i === 0 ? '今天' : '--', x, forecastY + 52, 16, 0x8fa5af));\n"
  "  dayWeather.push(ui.text('--', x, forecastY + 87, 20, 0xeaf3f5));\n"
  "  dayRange.push(ui.text('--度 / --度', x, forecastY + 127, 16, 0x9eb1ba));\n"
  "}\n"
  "function setText(h, s) { ui.setText(h, String(s)); }\n"
  "function value(v) { const n = Number(v); return isFinite(n) ? Math.round(n) : '--'; }\n"
  "function clearWeather() {\n"
  "  setText(temp, '--'); setText(weather, '正在更新');\n"
  "  setText(range, '最高 --度  最低 --度');\n"
  "  setText(humidity, '--'); setText(wind, '--'); setText(power, '--');\n"
  "  for (let i = 0; i < 5; i++) { setText(dayName[i], i === 0 ? '今天' : '--'); setText(dayWeather[i], '--'); setText(dayRange[i], '--度 / --度'); }\n"
  "}\n"
  "function tone(wx) {\n"
  "  const s = wx || '';\n"
  "  if (s.indexOf('雷') >= 0) return 0x392f51;\n"
  "  if (s.indexOf('雪') >= 0) return 0x405b68;\n"
  "  if (s.indexOf('雨') >= 0) return 0x173a52;\n"
  "  if (s.indexOf('阴') >= 0) return 0x3b4a50;\n"
  "  if (s.indexOf('云') >= 0) return 0x245063;\n"
  "  if (s.indexOf('晴') >= 0) return 0x11657a;\n"
  "  if (s.indexOf('雾') >= 0 || s.indexOf('霾') >= 0) return 0x514f49;\n"
  "  return 0x123e4a;\n"
  "}\n"
  "function render(r) {\n"
  "  let w;\n"
  "  try { w = JSON.parse(r.data); } catch (e) { throw new Error('天气数据格式错误'); }\n"
  "  if (!w || w.temperature === undefined) { throw new Error('找不到该城市'); }\n"
  "  setText(current, w.district || w.city || cities[cityIndex].n);\n"
  "  setText(place, (w.province || '中国') + (w.city ? ' · ' + w.city : ''));\n"
  "  setText(temp, value(w.temperature));\n"
  "  setText(weather, w.weather || '—');\n"
  "  ui.setColor(hero, tone(w.weather));\n"
  "  setText(range, '最高 ' + value(w.temp_max) + '度  最低 ' + value(w.temp_min) + '度');\n"
  "  setText(humidity, w.humidity === undefined ? '--' : w.humidity + '%');\n"
  "  setText(wind, w.wind_direction || '—');\n"
  "  setText(power, w.wind_power || '—');\n"
  "  setText(state, w.report_time ? '更新于 ' + w.report_time : '刚刚更新');\n"
  "  const f = w.forecast || [];\n"
  "  for (let i = 0; i < 5; i++) {\n"
  "    const d = f[i];\n"
  "    const wk = d && d.week ? d.week.slice(-1) : '--';\n"
  "    setText(dayName[i], d ? (i === 0 ? '今天' : '周' + wk) : '--');\n"
  "    setText(dayWeather[i], d ? (d.weather_day || '—') : '--');\n"
  "    setText(dayRange[i], d ? value(d.temp_max) + '度 / ' + value(d.temp_min) + '度' : '--度 / --度');\n"
  "  }\n"
  "  hasWeather = true;\n"
  "}\n"
  "function refresh() {\n"
  "  if (loading) return;\n"
  "  const n = network.status();\n"
  "  if (!n.connected) { setText(state, 'Wi-Fi 未连接'); prompt.showToast({message:'请先连接网络'}); return; }\n"
  "  loading = true;\n"
  "  const id = ++requestId;\n"
  "  const query = cities[cityIndex].q;\n"
  "  setText(state, hasWeather ? '正在刷新天气' : '正在同步时间和天气数据');\n"
  "  fetch.fetch({url:'https://uapis.cn/api/v1/misc/weather?city=' + encodeURIComponent(query) + '&forecast=true', timeout:20000}).then(function (r) { if (id !== requestId) return; render(r); loading = false; }).catch(function (e) { if (id !== requestId) return; const code = e && e.code !== undefined ? e.code : '?'; loading = false; setText(state, hasWeather ? '更新失败 · 显示上次结果' : '天气服务暂时不可用 (' + code + ')'); prompt.showToast({message:'天气更新失败'}); });\n"
  "}\n"
  "function saveCities() {\n"
  "  storage.set('cities', JSON.stringify(cities));\n"
  "  storage.set('cur', cities[cityIndex].q);\n"
  "}\n"
  "function validPinyin(q) {\n"
  "  if (!q || q.length > 24) return false;\n"
  "  for (let i = 0; i < q.length; i++) { const c = q.charCodeAt(i); if (c < 97 || c > 122) return false; }\n"
  "  return true;\n"
  "}\n"
  "const refreshButton = ui.button('刷新天气', actionX, 24, 150, 44, refresh, 0xe6b84e);\n"
  "let cityOverlay, cityTitle, cityHint, cityBack, cityAdd, cityDelete;\n"
  "const cityButtons = [];\n"
  "function setCityScreen(hidden) {\n"
  "  const list = [cityOverlay,cityTitle,cityHint,cityBack,cityAdd,cityDelete].concat(cityButtons);\n"
  "  for (let i = 0; i < list.length; i++) ui.setHidden(list[i], hidden);\n"
  "}\n"
  "function updateCityList() {\n"
  "  for (let i = 0; i < cityButtons.length; i++) {\n"
  "    if (i < cities.length) { setText(cityButtons[i], (i === cityIndex ? '当前  ' : '') + cities[i].n + '  ' + cities[i].q); ui.setHidden(cityButtons[i], false); }\n"
  "    else ui.setHidden(cityButtons[i], true);\n"
  "  }\n"
  "  setText(cityHint, cities.length + ' / 8 个城市 · 点击城市即可切换');\n"
  "}\n"
  "function chooseCity(i) {\n"
  "  if (i < 0 || i >= cities.length) return;\n"
  "  cityIndex = i; saveCities(); updateCityList(); setCityScreen(true);\n"
  "  requestId++; loading = false; hasWeather = false;\n"
  "  setText(current, cities[cityIndex].n); setText(place, '正在获取城市信息'); clearWeather(); refresh();\n"
  "}\n"
  "function openCities() { updateCityList(); setCityScreen(false); }\n"
  "function addCity() {\n"
  "  if (cities.length >= 8) { prompt.showToast({message:'最多添加 8 个城市'}); return; }\n"
  "  prompt.input({title:'添加城市',placeholder:'输入小写拼音，例如 shanghai',maxLength:24}, function (text) {\n"
  "    const q = String(text || '').trim().toLowerCase();\n"
  "    if (!validPinyin(q)) { prompt.showToast({message:'请输入城市的小写拼音'}); return; }\n"
  "    for (let i = 0; i < cities.length; i++) if (cities[i].q === q) { prompt.showToast({message:'这个城市已经添加'}); return; }\n"
  "    setText(cityHint, '正在查找 ' + q + ' ...');\n"
  "    fetch.fetch({url:'https://uapis.cn/api/v1/misc/weather?city=' + encodeURIComponent(q),timeout:20000}).then(function (r) {\n"
  "      let w; try { w = JSON.parse(r.data); } catch (e) { w = null; }\n"
  "      if (!w || (!w.weather && w.temperature === undefined)) { updateCityList(); prompt.showToast({message:'找不到这个城市'}); return; }\n"
  "      const name = w.district || w.city || q; cities.push({q:q,n:name}); cityIndex = cities.length - 1; saveCities(); updateCityList(); setCityScreen(true);\n"
  "      requestId++; loading = false; hasWeather = false; setText(current,name); setText(place,'正在获取城市信息'); clearWeather(); refresh(); prompt.showToast({message:'已添加 ' + name});\n"
  "    }).catch(function () { updateCityList(); prompt.showToast({message:'城市查询失败'}); });\n"
  "  });\n"
  "}\n"
  "function deleteCity() {\n"
  "  if (cities.length <= 1) { prompt.showToast({message:'至少保留一个城市'}); return; }\n"
  "  const removed = cities[cityIndex].n; cities.splice(cityIndex,1); if (cityIndex >= cities.length) cityIndex = cities.length - 1; saveCities(); updateCityList();\n"
  "  setText(current,cities[cityIndex].n); prompt.showToast({message:'已删除 ' + removed});\n"
  "}\n"
  "const manageButton = ui.button('管理城市', actionX, 82, 150, 44, openCities, 0x1d5362);\n"
  "cityOverlay = ui.panel(0, 0, W, H, 0x081218, 0, 255);\n"
  "cityTitle = ui.text('城市列表', 32, 22, 28, 0xf4fbfc);\n"
  "cityHint = ui.text('', 34, 68, 16, 0x8fa5af);\n"
  "const cityW = Math.floor((W - 80) / 2);\n"
  "for (let i = 0; i < 8; i++) {\n"
  "  const x = 32 + (i % 2) * (cityW + 16), y = 110 + Math.floor(i / 2) * 72;\n"
  "  cityButtons.push(ui.button('', x, y, cityW, 58, function () { chooseCity(i); }, 0x173541));\n"
  "}\n"
  "cityBack = ui.button('返回天气', 32, H - 68, 150, 46, function () { setCityScreen(true); }, 0x40515a);\n"
  "cityDelete = ui.button('删除当前城市', W - 388, H - 68, 176, 46, deleteCity, 0x6f3941);\n"
  "cityAdd = ui.button('添加城市', W - 196, H - 68, 164, 46, addCity, 0x16758a);\n"
  "updateCityList(); setCityScreen(true);\n"
  "refresh();\n"
  "setInterval(refresh, 600000);\n";

static const char g_2048_qpk_js[] =
  "'use strict';\n"
  "import storage from '@system.storage';\n"
  "const viewport = ui.getSize();\n"
  "const W = viewport.width, H = viewport.height;\n"
  "const controlsY = H - 48;\n"
  "const tileGap = 8;\n"
  "const boardSize = Math.min(W - 40, controlsY - 54);\n"
  "const tileSize = Math.floor((boardSize - 16 - tileGap * 3) / 4);\n"
  "const actualBoard = tileSize * 4 + tileGap * 3 + 16;\n"
  "const boardX = Math.floor((W - actualBoard) / 2);\n"
  "const boardY = 46;\n"
  "const scoreLabel = ui.text('分数 0', 20, 10, 20, 0x776e65);\n"
  "const bestLabel = ui.text('最高 0', 150, 10, 20, 0x776e65);\n"
  "const statusLabel = ui.text('准备开始', W - 170, 12, 16, 0x776e65);\n"
  "ui.background(0xfaf8ef);\n"
  "const boardPanel = ui.panel(boardX, boardY, actualBoard, actualBoard, 0xbbada0, 10, 255);\n"
  "const tilePanels = [], tileLabels = [];\n"
  "for (let i = 0; i < 16; i++) {\n"
  "  const x = boardX + 8 + (i % 4) * (tileSize + tileGap);\n"
  "  const y = boardY + 8 + Math.floor(i / 4) * (tileSize + tileGap);\n"
  "  tilePanels.push(ui.panel(x, y, tileSize, tileSize, 0xcdc1b4, 7, 255));\n"
  "  tileLabels.push(ui.number('', x, y, tileSize, tileSize, 0x776e65));\n"
  "}\n"
  "const board = [];\n"
  "let score = 0;\n"
  "let best = 0;\n"
  "let gameOver = false;\n"
  "let won = false;\n"
  "try { const value = Number(storage.get('best') || 0); if (isFinite(value) && value > 0) best = Math.floor(value); } catch (e) {}\n"
  "function setText(handle, value) { ui.setText(handle, String(value)); }\n"
  "function tileColor(value) {\n"
  "  if (value === 2) return 0xeee4da;\n"
  "  if (value === 4) return 0xede0c8;\n"
  "  if (value === 8) return 0xf2b179;\n"
  "  if (value === 16) return 0xf59563;\n"
  "  if (value === 32) return 0xf67c5f;\n"
  "  if (value === 64) return 0xf65e3b;\n"
  "  if (value === 128) return 0xedcf72;\n"
  "  if (value === 256) return 0xedcc61;\n"
  "  if (value === 512) return 0xedc850;\n"
  "  if (value === 1024) return 0xedc53f;\n"
  "  if (value >= 2048) return 0xedc22e;\n"
  "  return 0xcdc1b4;\n"
  "}\n"
  "function textColor(value) { return value <= 4 ? 0x776e65 : 0xffffff; }\n"
  "function addTile() {\n"
  "  const empty = [];\n"
  "  for (let i = 0; i < 16; i++) if (board[i] === 0) empty.push(i);\n"
  "  if (!empty.length) return;\n"
  "  const index = empty[Math.floor(Math.random() * empty.length)];\n"
  "  board[index] = Math.random() < 0.9 ? 2 : 4;\n"
  "}\n"
  "function reset() {\n"
  "  board.length = 0;\n"
  "  for (let i = 0; i < 16; i++) board.push(0);\n"
  "  score = 0; gameOver = false; won = false; addTile(); addTile(); draw();\n"
  "}\n"
  "function slide(line) {\n"
  "  const values = [];\n"
  "  const result = [];\n"
  "  for (let i = 0; i < 4; i++) if (line[i]) values.push(line[i]);\n"
  "  for (let i = 0; i < values.length; i++) {\n"
  "    if (i + 1 < values.length && values[i] === values[i + 1]) {\n"
  "      const merged = values[i] * 2; result.push(merged); score += merged;\n"
  "      if (merged === 2048) won = true; i++;\n"
  "    } else result.push(values[i]);\n"
  "  }\n"
  "  while (result.length < 4) result.push(0);\n"
  "  for (let i = 0; i < 4; i++) if (result[i] !== line[i]) return {line:result, changed:true};\n"
  "  return {line:result, changed:false};\n"
  "}\n"
  "function move(direction) {\n"
  "  if (gameOver) return;\n"
  "  let changed = false;\n"
  "  for (let n = 0; n < 4; n++) {\n"
  "    const line = [];\n"
  "    for (let k = 0; k < 4; k++) {\n"
  "      const source = direction === 'right' || direction === 'down' ? 3 - k : k;\n"
  "      const p = direction === 'left' || direction === 'right' ? n * 4 + source : source * 4 + n;\n"
  "      line[k] = board[p];\n"
  "    }\n"
  "    const result = slide(line);\n"
  "    if (result.changed) changed = true;\n"
  "    for (let k = 0; k < 4; k++) {\n"
  "      const target = direction === 'right' || direction === 'down' ? 3 - k : k;\n"
  "      const p = direction === 'left' || direction === 'right' ? n * 4 + target : target * 4 + n;\n"
  "      board[p] = result.line[k];\n"
  "    }\n"
  "  }\n"
  "  if (!changed) { if (!canMove()) { gameOver = true; setText(statusLabel, '游戏结束'); } return; }\n"
  "  if (score > best) { best = score; try { storage.set('best', String(best)); } catch (e) {} }\n"
  "  addTile();\n"
  "  if (!canMove()) gameOver = true;\n"
  "  draw();\n"
  "}\n"
  "function canMove() {\n"
  "  for (let i = 0; i < 16; i++) {\n"
  "    if (board[i] === 0) return true;\n"
  "    if (i % 4 < 3 && board[i] === board[i + 1]) return true;\n"
  "    if (i < 12 && board[i] === board[i + 4]) return true;\n"
  "  }\n"
  "  return false;\n"
  "}\n"
  "function draw() {\n"
  "  setText(scoreLabel, '分数 ' + score); setText(bestLabel, '最高 ' + best);\n"
  "  if (gameOver) setText(statusLabel, '游戏结束'); else if (won) setText(statusLabel, '达成 2048'); else setText(statusLabel, '准备开始');\n"
  "  for (let i = 0; i < 16; i++) { const value = board[i]; ui.setColor(tilePanels[i], tileColor(value)); ui.setColor(tileLabels[i], textColor(value)); setText(tileLabels[i], value ? value : ''); }\n"
  "}\n"
  "const restart = ui.button('重新开始', 20, controlsY, 132, 42, reset, 0xf0a04b);\n"
  "const left = ui.button('左', W - 292, controlsY, 62, 42, function () { move('left'); }, 0x776e65);\n"
  "const up = ui.button('上', W - 224, controlsY, 62, 42, function () { move('up'); }, 0x776e65);\n"
  "const down = ui.button('下', W - 156, controlsY, 62, 42, function () { move('down'); }, 0x776e65);\n"
  "const right = ui.button('右', W - 88, controlsY, 62, 42, function () { move('right'); }, 0x776e65);\n"
  "ui.onSwipe(function (direction) { move(direction); });\n"
  "reset();\n";

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

static char *qpk_read_js(const char *filename, size_t *source_size)
{
  FILE *file;
  struct stat st;
  char *source;
  size_t got;

  if (stat(filename, &st) < 0 || !S_ISREG(st.st_mode) ||
      st.st_size <= 0 || st.st_size > 128 * 1024)
    {
      return NULL;
    }

  file = fopen(filename, "rb");
  if (file == NULL)
    {
      return NULL;
    }

  source = malloc((size_t)st.st_size + 1);
  if (source == NULL)
    {
      fclose(file);
      return NULL;
    }

  got = fread(source, 1, (size_t)st.st_size, file);
  fclose(file);
  if (got != (size_t)st.st_size)
    {
      free(source);
      return NULL;
    }

  source[got] = '\0';
  *source_size = got;
  return source;
}

static char *qpk_load_entry(const struct qpk_entry_s *qpk,
                            char *filename, size_t filename_size,
                            size_t *source_size)
{
  const char *entry = qpk->entry[0] ? qpk->entry : "pages/index";
  char *source;
  int i;
  const char *patterns[] = {
    "%s/%s/%s/index.js",
    "%s/%s/%s.js",
    "%s/%s/pages/index/index.js",
    "%s/%s/app.js"
  };

  for (i = 0; i < 4; i++)
    {
      if (i < 2)
        {
          if (snprintf(filename, filename_size, patterns[i], QPK_DIR,
                       qpk->dir, entry) >= (int)filename_size)
            {
              continue;
            }
        }
      else if (snprintf(filename, filename_size, patterns[i], QPK_DIR,
                        qpk->dir) >= (int)filename_size)
        {
          continue;
        }

      source = qpk_read_js(filename, source_size);
      if (source != NULL)
        {
          printf("[qpk] entry file %s (%u bytes)\n", filename,
                 (unsigned int)*source_size);
          return source;
        }
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

static void qapp_gesture(lv_event_t *e)
{
  lv_indev_t *indev = lv_event_get_indev(e);
  lv_dir_t expected = (lv_dir_t)(uintptr_t)lv_event_get_user_data(e);

  if (indev == NULL || lv_indev_get_gesture_dir(indev) != expected)
    {
      return;
    }

  lv_indev_wait_release(indev);
  lv_event_stop_processing(e);
  qapp_back(NULL);
}

static lv_obj_t *qapp_page(const char *title)
{
  lv_obj_t *page;
  lv_obj_t *content;
  lv_obj_t *back;
  lv_obj_t *left_edge;
  lv_obj_t *right_edge;
  lv_obj_t *label;
  int height;

  qpk_runtime_stop();
  lv_obj_remove_flag(g_desktop.panel, LV_OBJ_FLAG_HIDDEN);
  panel_content_forget();
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
  lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE |
                              LV_OBJ_FLAG_GESTURE_BUBBLE);

  left_edge = lv_obj_create(page);
  lv_obj_set_pos(left_edge, 0, QAPP_HEADER_HEIGHT);
  lv_obj_set_size(left_edge, 36, height);
  lv_obj_set_style_bg_opa(left_edge, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(left_edge, 0, 0);
  lv_obj_set_style_pad_all(left_edge, 0, 0);
  lv_obj_remove_flag(left_edge, LV_OBJ_FLAG_SCROLLABLE |
                                LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_event_cb(left_edge, qapp_gesture, LV_EVENT_GESTURE,
                     (void *)(uintptr_t)LV_DIR_RIGHT);

  right_edge = lv_obj_create(page);
  lv_obj_set_size(right_edge, 36, height);
  lv_obj_align(right_edge, LV_ALIGN_TOP_RIGHT, 0, QAPP_HEADER_HEIGHT);
  lv_obj_set_style_bg_opa(right_edge, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(right_edge, 0, 0);
  lv_obj_set_style_pad_all(right_edge, 0, 0);
  lv_obj_remove_flag(right_edge, LV_OBJ_FLAG_SCROLLABLE |
                                 LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_event_cb(right_edge, qapp_gesture, LV_EVENT_GESTURE,
                     (void *)(uintptr_t)LV_DIR_LEFT);

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
                     sizeof(g_hello_qpk_js) - 1, zh_font, number_font,
                     show_toast,
                     qpk_show_dialog);
}

static void launch_builtin_2048_qapp(lv_event_t *e)
{
  lv_obj_t *card;
  const struct qpk_entry_s *manifest = &g_builtin_2048_qpk.manifest;

  LV_UNUSED(e);
  card = qapp_page(manifest->name);
  qpk_runtime_launch(card, manifest->name, manifest->package,
                     manifest->version, manifest->entry, g_2048_qpk_js,
                     sizeof(g_2048_qpk_js) - 1, zh_font, number_font,
                     show_toast,
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
                                   source_size, zh_font, number_font,
                                   show_toast,
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
                             const char *subtitle, int y, int width,
                             lv_event_cb_t cb, void *user)
{
  lv_obj_t *button = lv_button_create(parent);
  lv_obj_t *label;

  lv_obj_set_size(button, width, 72);
  lv_obj_align(button, LV_ALIGN_TOP_LEFT, 24, y);
  lv_obj_set_style_bg_color(button, lv_color_hex(theme_surface()), 0);
  lv_obj_set_style_radius(button, 14, 0);
  lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, user);
  label = make_label(button, title, theme_primary(), 20);
  lv_obj_align(label, LV_ALIGN_TOP_LEFT, 12, 1);
  label = make_label(button, subtitle, theme_secondary(), 16);
  lv_obj_align(label, LV_ALIGN_BOTTOM_LEFT, 12, -1);
  return button;
}

static bool qpk_dir_safe(const char *dir)
{
  size_t i;

  if (dir == NULL || dir[0] == '\0' || dir[0] == '.')
    {
      return false;
    }

  for (i = 0; dir[i] != '\0'; i++)
    {
      if (dir[i] == '/' || dir[i] == '\\' || dir[i] == ':')
        {
          return false;
        }
    }

  return true;
}

static int path_rmtree(const char *path)
{
  for (; ; )
    {
      DIR *dp;
      struct dirent *ent;
      struct stat st;
      char child[192];
      bool found = false;

      if (lstat(path, &st) < 0)
        {
          return -1;
        }

      if (!S_ISDIR(st.st_mode))
        {
          return unlink(path);
        }

      dp = opendir(path);
      if (dp == NULL)
        {
          return -1;
        }

      while ((ent = readdir(dp)) != NULL)
        {
          if (ent->d_name[0] == '.' &&
              (ent->d_name[1] == '\0' ||
               (ent->d_name[1] == '.' && ent->d_name[2] == '\0')))
            {
              continue;
            }

          if (snprintf(child, sizeof(child), "%s/%s", path, ent->d_name) >=
              (int)sizeof(child))
            {
              closedir(dp);
              errno = ENAMETOOLONG;
              return -1;
            }

          found = true;
          break;
        }

      closedir(dp);
      if (!found)
        {
          break;
        }

      if (path_rmtree(child) < 0)
        {
          return -1;
        }
    }

  return rmdir(path);
}

static void qpk_delete_confirm_close(lv_event_t *e)
{
  lv_obj_delete(lv_event_get_user_data(e));
}

static void qpk_delete_do(lv_event_t *e)
{
  lv_obj_t *shade = lv_event_get_user_data(e);
  char path[192];
  int ret;

  if (!qpk_dir_safe(g_qpk_delete_dir) ||
      snprintf(path, sizeof(path), "%s/%s", QPK_DIR, g_qpk_delete_dir) >=
      (int)sizeof(path))
    {
      if (shade != NULL)
        {
          lv_obj_delete(shade);
        }

      show_toast("无法删除该应用");
      return;
    }

  ret = path_rmtree(path);
  g_qpk_delete_dir[0] = '\0';
  if (ret < 0)
    {
      if (shade != NULL)
        {
          lv_obj_delete(shade);
        }

      show_toast("删除失败");
      return;
    }

  qpk_clicked(NULL);
  show_toast("已删除");
}

static void qpk_delete_clicked(lv_event_t *e)
{
  intptr_t index = (intptr_t)lv_event_get_user_data(e);
  struct qpk_entry_s *qpk;
  lv_obj_t *shade;
  lv_obj_t *box;
  lv_obj_t *button;
  lv_obj_t *label;
  char text[192];

  if (index < 0 || index >= g_desktop.nqpk)
    {
      return;
    }

  qpk = &g_desktop.qpk[index];
  if (!qpk_dir_safe(qpk->dir))
    {
      show_toast("无法删除该应用");
      return;
    }

  strlcpy(g_qpk_delete_dir, qpk->dir, sizeof(g_qpk_delete_dir));
  strlcpy(g_qpk_delete_name, qpk->name, sizeof(g_qpk_delete_name));
  snprintf(text, sizeof(text),
           "确定删除「%s」？\n将移除 %s/%s 下的全部文件。",
           g_qpk_delete_name[0] ? g_qpk_delete_name : g_qpk_delete_dir,
           QPK_DIR, g_qpk_delete_dir);

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
  label = make_label(box, text, theme_primary(), 20);
  lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(label, 440);
  lv_obj_align(label, LV_ALIGN_TOP_LEFT, 12, 12);

  button = lv_button_create(box);
  lv_obj_set_size(button, 96, 42);
  lv_obj_align(button, LV_ALIGN_BOTTOM_LEFT, 8, -8);
  lv_obj_set_style_bg_color(button, lv_color_hex(theme_surface()), 0);
  lv_obj_add_event_cb(button, qpk_delete_confirm_close, LV_EVENT_CLICKED,
                      shade);
  label = make_label(button, "取消", theme_primary(), 20);
  lv_obj_center(label);

  button = lv_button_create(box);
  lv_obj_set_size(button, 96, 42);
  lv_obj_align(button, LV_ALIGN_BOTTOM_RIGHT, -8, -8);
  lv_obj_set_style_bg_color(button, lv_color_hex(0xc83d4b), 0);
  lv_obj_add_event_cb(button, qpk_delete_do, LV_EVENT_CLICKED, shade);
  label = make_label(button, "删除", 0xffffff, 20);
  lv_obj_center(label);
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
  list_button(card, g_builtin_qpk.manifest.name, builtin_subtitle, 66, 690,
              launch_builtin_qapp, NULL);

  snprintf(builtin_subtitle, sizeof(builtin_subtitle), "%s · %s",
           g_builtin_2048_qpk.kind, g_builtin_2048_qpk.format);
  list_button(card, g_builtin_2048_qpk.manifest.name, builtin_subtitle, 146,
              690, launch_builtin_2048_qapp, NULL);

  for (i = 0; i < g_desktop.nqpk && i < 4; i++)
    {
      char subtitle[96];
      lv_obj_t *del;
      lv_obj_t *label;
      int y = 226 + i * 80;

      snprintf(subtitle, sizeof(subtitle), "%s · %s",
               g_desktop.qpk[i].package[0] ? g_desktop.qpk[i].package :
               g_desktop.qpk[i].dir,
               g_desktop.qpk[i].version[0] ? g_desktop.qpk[i].version :
               "QPK");
      list_button(card, g_desktop.qpk[i].name, subtitle, y, 586,
                  external_qapp_clicked, (void *)(intptr_t)i);

      del = lv_button_create(card);
      lv_obj_set_size(del, 92, 72);
      lv_obj_align(del, LV_ALIGN_TOP_RIGHT, -24, y);
      lv_obj_set_style_bg_color(del, lv_color_hex(0xc83d4b), 0);
      lv_obj_set_style_radius(del, 14, 0);
      lv_obj_add_event_cb(del, qpk_delete_clicked, LV_EVENT_CLICKED,
                          (void *)(intptr_t)i);
      label = make_label(del, "删除", 0xffffff, 20);
      lv_obj_center(label);
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

static void wifi_status_text(FAR const struct wifi_snapshot_s *snapshot,
                             FAR char *text, size_t textlen)
{
  FAR const char *network = snapshot->ssid[0] != '\0' ?
                            snapshot->ssid : "已保存的网络";

  switch (snapshot->state)
    {
      case WIFI_STATE_STARTING:
        strlcpy(text, "正在启动无线网络…", textlen);
        break;
      case WIFI_STATE_ASSOCIATING:
        snprintf(text, textlen, "正在连接 %s…", network);
        break;
      case WIFI_STATE_DHCP:
        strlcpy(text, "已关联，正在获取 IP 地址…", textlen);
        break;
      case WIFI_STATE_CONNECTED:
        if (snapshot->ip[0] != '\0')
          {
            snprintf(text, textlen, "%s · %s", network, snapshot->ip);
          }
        else
          {
            snprintf(text, textlen, "%s · 已连接", network);
          }
        break;
      case WIFI_STATE_FAILED:
        snprintf(text, textlen, "连接失败（%d）", snapshot->result);
        break;
      case WIFI_STATE_IDLE:
      default:
#if DESKTOP_WIFI_ENABLED
        strlcpy(text, "尚未连接", textlen);
#else
        strlcpy(text, "当前固件未启用 Wi-Fi", textlen);
#endif
        break;
    }
}

static void wifi_ui_timer_cb(lv_timer_t *timer)
{
  struct wifi_snapshot_s snapshot;
  uint32_t color;
  char text[128];

  LV_UNUSED(timer);
  wifi_get_snapshot(&snapshot);
  wifi_status_text(&snapshot, text, sizeof(text));

  if (snapshot.state == WIFI_STATE_CONNECTED)
    {
      color = 0x55d6a7;
    }
  else if (snapshot.state == WIFI_STATE_FAILED)
    {
      color = 0xf06f75;
    }
  else if (snapshot.busy)
    {
      color = 0xf2b56b;
    }
  else
    {
      color = 0x9aa7cc;
    }

  if (g_desktop.wifi_icon != NULL)
    {
      lv_obj_set_style_text_color(g_desktop.wifi_icon,
                                  lv_color_hex(color), 0);
    }

  if (g_desktop.wifi_status_label != NULL)
    {
      lv_label_set_text(g_desktop.wifi_status_label, text);
      lv_obj_set_style_text_color(g_desktop.wifi_status_label,
                                  lv_color_hex(color), 0);
    }
}

static void wifi_input_focused(lv_event_t *e)
{
  if (g_desktop.wifi_keyboard != NULL)
    {
      lv_keyboard_set_textarea(g_desktop.wifi_keyboard,
                               lv_event_get_target(e));
    }
}

static void wifi_modal_close(lv_event_t *e)
{
  lv_obj_t *modal = g_desktop.wifi_modal;

  LV_UNUSED(e);
  wifi_modal_forget();
  if (modal != NULL)
    {
      lv_obj_delete(modal);
    }
}

static void wifi_connect_clicked(lv_event_t *e)
{
  FAR const char *password_text;
  FAR const char *ssid_text;
  char ssid[WIFI_SSID_MAXLEN + 1];
  char password[WIFI_PASSWORD_MAXLEN + 1];
  int ret;

  LV_UNUSED(e);
  if (g_desktop.wifi_ssid_input == NULL ||
      g_desktop.wifi_password_input == NULL)
    {
      return;
    }

  ssid_text = lv_textarea_get_text(g_desktop.wifi_ssid_input);
  password_text = lv_textarea_get_text(g_desktop.wifi_password_input);
  if (ssid_text[0] == '\0')
    {
      show_toast("请输入 Wi-Fi 名称");
      return;
    }

  if (strlen(ssid_text) > WIFI_SSID_MAXLEN ||
      strlen(password_text) > WIFI_PASSWORD_MAXLEN)
    {
      show_toast("Wi-Fi 名称或密码过长");
      return;
    }

  strlcpy(ssid, ssid_text, sizeof(ssid));
  strlcpy(password, password_text, sizeof(password));

  ret = wifi_start_connect(ssid, password);
  memset(password, 0, sizeof(password));
  if (ret < 0)
    {
      show_toast(ret == -EBUSY ? "正在连接，请稍候" : "无法启动 Wi-Fi 连接");
      return;
    }

  wifi_modal_close(NULL);
  show_toast("正在连接 Wi-Fi");
}

static void wifi_config_clicked(lv_event_t *e)
{
  struct wifi_snapshot_s snapshot;
  lv_obj_t *modal;
  lv_obj_t *dialog;
  lv_obj_t *label;
  lv_obj_t *button;

  LV_UNUSED(e);
  if (g_desktop.wifi_modal != NULL)
    {
      return;
    }

  wifi_get_snapshot(&snapshot);
  modal = lv_obj_create(g_desktop.panel);
  g_desktop.wifi_modal = modal;
  lv_obj_set_size(modal, lv_pct(100), lv_pct(100));
  lv_obj_set_style_bg_color(modal, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(modal, LV_OPA_80, 0);
  lv_obj_set_style_border_width(modal, 0, 0);
  lv_obj_set_style_radius(modal, 0, 0);
  lv_obj_set_style_pad_all(modal, 0, 0);
  lv_obj_remove_flag(modal, LV_OBJ_FLAG_SCROLLABLE);

  dialog = lv_obj_create(modal);
  lv_obj_set_size(dialog, 700, 315);
  lv_obj_align(dialog, LV_ALIGN_TOP_MID, 0, 10);
  lv_obj_set_style_bg_color(dialog, lv_color_hex(theme_card()), 0);
  lv_obj_set_style_border_color(dialog,
                                lv_color_hex(g_desktop.light_theme ?
                                             0xcbd1dd : 0x35415e), 0);
  lv_obj_set_style_radius(dialog, 16, 0);
  lv_obj_remove_flag(dialog, LV_OBJ_FLAG_SCROLLABLE);

  label = make_label(dialog, "连接 Wi-Fi", theme_primary(), 28);
  lv_obj_set_pos(label, 20, 8);

  label = make_label(dialog, "网络名称", theme_secondary(), 16);
  lv_obj_set_pos(label, 22, 66);
  g_desktop.wifi_ssid_input = lv_textarea_create(dialog);
  lv_obj_set_size(g_desktop.wifi_ssid_input, 510, 48);
  lv_obj_set_pos(g_desktop.wifi_ssid_input, 145, 54);
  lv_textarea_set_one_line(g_desktop.wifi_ssid_input, true);
  lv_textarea_set_max_length(g_desktop.wifi_ssid_input, WIFI_SSID_MAXLEN);
  lv_textarea_set_placeholder_text(g_desktop.wifi_ssid_input, "SSID");
  lv_textarea_set_text(g_desktop.wifi_ssid_input, snapshot.ssid);
  lv_obj_set_style_text_font(g_desktop.wifi_ssid_input, zh_font(20), 0);
  lv_obj_add_event_cb(g_desktop.wifi_ssid_input, wifi_input_focused,
                      LV_EVENT_FOCUSED, NULL);

  label = make_label(dialog, "密码", theme_secondary(), 16);
  lv_obj_set_pos(label, 22, 128);
  g_desktop.wifi_password_input = lv_textarea_create(dialog);
  lv_obj_set_size(g_desktop.wifi_password_input, 510, 48);
  lv_obj_set_pos(g_desktop.wifi_password_input, 145, 116);
  lv_textarea_set_one_line(g_desktop.wifi_password_input, true);
  lv_textarea_set_max_length(g_desktop.wifi_password_input,
                             WIFI_PASSWORD_MAXLEN);
  lv_textarea_set_password_mode(g_desktop.wifi_password_input, true);
  lv_textarea_set_placeholder_text(g_desktop.wifi_password_input,
                                   "Wi-Fi 密码");
  lv_obj_set_style_text_font(g_desktop.wifi_password_input, zh_font(20), 0);
  lv_obj_add_event_cb(g_desktop.wifi_password_input, wifi_input_focused,
                      LV_EVENT_FOCUSED, NULL);

  button = lv_button_create(dialog);
  lv_obj_set_size(button, 150, 48);
  lv_obj_set_pos(button, 345, 192);
  lv_obj_set_style_bg_color(button, lv_color_hex(theme_surface()), 0);
  lv_obj_add_event_cb(button, wifi_modal_close, LV_EVENT_CLICKED, NULL);
  label = make_label(button, "取消", theme_primary(), 20);
  lv_obj_center(label);

  button = lv_button_create(dialog);
  lv_obj_set_size(button, 150, 48);
  lv_obj_set_pos(button, 505, 192);
  lv_obj_set_style_bg_color(button, lv_color_hex(0x5267d8), 0);
  lv_obj_add_event_cb(button, wifi_connect_clicked, LV_EVENT_CLICKED, NULL);
  label = make_label(button, "连接", 0xffffff, 20);
  lv_obj_center(label);

  g_desktop.wifi_keyboard = lv_keyboard_create(modal);
  lv_obj_set_size(g_desktop.wifi_keyboard, lv_pct(100), 255);
  lv_obj_align(g_desktop.wifi_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_keyboard_set_textarea(g_desktop.wifi_keyboard,
                           g_desktop.wifi_ssid_input);
}

static void filemgr_clicked(lv_event_t *e)
{
  struct wifi_snapshot_s snapshot;
  lv_obj_t *card;
  lv_obj_t *label;
  char token[8];
  char text[320];
  int ret;

  LV_UNUSED(e);
  wifi_get_snapshot(&snapshot);
  card = panel_card("文件管理");
  if (snapshot.state != WIFI_STATE_CONNECTED || snapshot.ip[0] == '\0')
    {
      snprintf(text, sizeof(text),
               "请先连接 Wi-Fi 并获取 IP 地址。\n\n"
               "连接后，局域网内的电脑或手机可以通过浏览器管理 /data。\n"
               "支持浏览、上传、下载、新建目录和删除。"
               "zip / rpk / qpk 在浏览器中解压后写入 /data。");
    }
  else
    {
      ret = desktop_filemgr_start();
      if (ret < 0 || desktop_filemgr_get_token(token, sizeof(token)) < 0)
        {
          snprintf(text, sizeof(text),
                   "文件管理服务启动失败（%d）。\n\n"
                   "请确认端口 %d 没有被占用。",
                   ret, CONFIG_SYSTEM_DESKTOP_FILEMGR_PORT);
        }
      else
        {
          snprintf(text, sizeof(text),
                   "文件管理服务已启动\n\n"
                   "浏览器地址：http://%s:%d/\n"
                   "访问码：%s\n\n"
                   "在同一个局域网内打开地址，首次访问输入访问码。\n"
                   "所有操作都限制在 /data 目录内。"
                   "zip / rpk / qpk 会在浏览器里解压，再把文件写入设备。",
                   snapshot.ip, CONFIG_SYSTEM_DESKTOP_FILEMGR_PORT, token);
        }
    }

  label = make_label(card, text, theme_secondary(), 20);
  lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(label, 690);
  lv_obj_set_pos(label, 24, 82);
}

static void camera_clicked(lv_event_t *e)
{
  lv_obj_t *card;
  lv_obj_t *label;
  int ret;

  LV_UNUSED(e);
  card = panel_card("摄像头");
  ret = desktop_camera_start(card, zh_font(20));
  if (ret < 0)
    {
      label = make_label(card, "无法启动摄像头", 0xff8e8e, 20);
      lv_obj_center(label);
    }
}

static void settings_clicked(lv_event_t *e)
{
  lv_obj_t *card;
  lv_obj_t *button;
  lv_obj_t *label;
  char info[256];
  char status[128];
  struct wifi_snapshot_s snapshot;

  LV_UNUSED(e);
  card = panel_card("设置");
  setting_row(card, "浅色桌面", "切换桌面背景与应用卡片", 68,
              g_desktop.light_theme, theme_changed);

  label = make_label(card, "Wi-Fi", theme_primary(), 20);
  lv_obj_set_pos(label, 28, 140);
  wifi_get_snapshot(&snapshot);
  wifi_status_text(&snapshot, status, sizeof(status));
  g_desktop.wifi_status_label = make_label(card, status,
                                            theme_secondary(), 16);
  lv_label_set_long_mode(g_desktop.wifi_status_label, LV_LABEL_LONG_DOT);
  lv_obj_set_size(g_desktop.wifi_status_label, 480, 28);
  lv_obj_set_pos(g_desktop.wifi_status_label, 28, 170);

  button = lv_button_create(card);
  lv_obj_set_size(button, 150, 50);
  lv_obj_set_pos(button, 558, 142);
  lv_obj_set_style_bg_color(button, lv_color_hex(0x5267d8), 0);
  lv_obj_add_event_cb(button, wifi_config_clicked, LV_EVENT_CLICKED, NULL);
  label = make_label(button, "配置", 0xffffff, 20);
  lv_obj_center(label);

  label = make_label(card, "文件管理", theme_primary(), 20);
  lv_obj_set_pos(label, 28, 212);
  label = make_label(card, "通过局域网浏览器管理 /data",
                     theme_secondary(), 16);
  lv_obj_set_pos(label, 28, 242);

  button = lv_button_create(card);
  lv_obj_set_size(button, 150, 50);
  lv_obj_set_pos(button, 558, 214);
  lv_obj_set_style_bg_color(button, lv_color_hex(0x31927a), 0);
  lv_obj_add_event_cb(button, filemgr_clicked, LV_EVENT_CLICKED, NULL);
  label = make_label(button, desktop_filemgr_running() ? "打开" : "启动",
                     0xffffff, 20);
  lv_obj_center(label);

  label = make_label(card, "摄像头", theme_primary(), 20);
  lv_obj_set_pos(label, 28, 284);
  label = make_label(card, "SC2336  1280 x 720", theme_secondary(), 16);
  lv_obj_set_pos(label, 28, 314);

  button = lv_button_create(card);
  lv_obj_set_size(button, 150, 50);
  lv_obj_set_pos(button, 558, 286);
  lv_obj_set_style_bg_color(button, lv_color_hex(0x5267d8), 0);
  lv_obj_add_event_cb(button, camera_clicked, LV_EVENT_CLICKED, NULL);
  label = make_label(button, "打开", 0xffffff, 20);
  lv_obj_center(label);

  snprintf(info, sizeof(info),
           "设备信息\nESP32-P4 Function-EV-Board\n"
           "双核 RISC-V · PSRAM 已启用 · 触摸已连接\n"
           "NuttX 桌面 · 已发现 %d 个外部 QPK",
           g_desktop.nqpk);
  label = make_label(card, info, theme_secondary(), 16);
  lv_obj_set_pos(label, 28, 360);

  wifi_ui_timer_cb(NULL);
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
  g_desktop.wifi_icon = lv_label_create(bar);
  lv_label_set_text(g_desktop.wifi_icon, LV_SYMBOL_WIFI);
  lv_obj_set_style_text_font(g_desktop.wifi_icon,
                             &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(g_desktop.wifi_icon,
                              lv_color_hex(0x9aa7cc), 0);
  lv_obj_align(g_desktop.wifi_icon, LV_ALIGN_RIGHT_MID, -132, 0);

  label = lv_label_create(bar);
  lv_label_set_text(label, LV_SYMBOL_CHARGE);
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
  size_t number_font_size =
    (size_t)(g_desktop_number_font_end - g_desktop_number_font_start);

  memset(&g_desktop, 0, sizeof(g_desktop));
  g_desktop.font16 = lv_tiny_ttf_create_data(g_desktop_font_start,
                                              font_size, 16);
  g_desktop.font20 = lv_tiny_ttf_create_data(g_desktop_font_start,
                                              font_size, 20);
  g_desktop.font28 = lv_tiny_ttf_create_data(g_desktop_font_start,
                                              font_size, 28);
  g_desktop.font48 = lv_tiny_ttf_create_data(g_desktop_font_start,
                                              font_size, 48);
  g_desktop.number_font32 =
    lv_tiny_ttf_create_data(g_desktop_number_font_start,
                            number_font_size, 32);
  g_desktop.number_font40 =
    lv_tiny_ttf_create_data(g_desktop_number_font_start,
                            number_font_size, 40);
  g_desktop.number_font48 =
    lv_tiny_ttf_create_data(g_desktop_number_font_start,
                            number_font_size, 48);
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
  lv_timer_create(wifi_ui_timer_cb, 500, NULL);
  clock_timer_cb(NULL);
  wifi_ui_timer_cb(NULL);
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

  if (wifi_start_connect("", "") < 0)
    {
      fprintf(stderr, "desktop: Wi-Fi auto-connect unavailable\n");
    }

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
