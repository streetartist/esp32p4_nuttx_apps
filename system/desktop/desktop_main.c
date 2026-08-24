/****************************************************************************
 * apps/system/desktop/desktop_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Phone-style desktop launcher for ESP32-P4 Function-EV-Board (NuttX).
 * Status bar + application grid.  Entries under CONFIG_SYSTEM_DESKTOP_QPK_DIR
 * that contain a manifest.json are listed as quick apps.
 *
 ****************************************************************************/

#include <nuttx/config.h>

#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/boardctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <nuttx/sched.h>
#include <lvgl/lvgl.h>

#define QPK_DIR     CONFIG_SYSTEM_DESKTOP_QPK_DIR
#define MAX_QPK     8
#define NAME_MAXLEN 32

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct qpk_entry_s
{
  char dir[64];
  char name[NAME_MAXLEN];
};

struct desktop_env_s
{
  lv_obj_t *clock_label;
  lv_obj_t *grid;
  lv_obj_t *panel;      /* overlay panel for built-in app pages */
  struct qpk_entry_s qpk[MAX_QPK];
  int nqpk;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct desktop_env_s g_desktop;

static const char *g_builtin_icons[] =
{
  LV_SYMBOL_SETTINGS,
  LV_SYMBOL_BELL,
  LV_SYMBOL_LIST,
};

static const char *g_builtin_names[] =
{
  "System",
  "Welcome",
  "Quick Apps",
};

enum builtin_id_e
{
  APP_ABOUT = 0,
  APP_HELLO,
  APP_QPK
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* Extract a top-level string field from a small JSON manifest without a
 * full parser: finds "field" then the following string value.
 */

static void json_get_string(const char *json, const char *field,
                            char *out, size_t outlen)
{
  const char *p;
  const char *end;

  out[0] = '\0';
  snprintf(out, outlen, "%s", "");

  p = strstr(json, field);
  if (p == NULL)
    {
      return;
    }

  p = strchr(p + strlen(field), '"');
  if (p == NULL)
    {
      return;
    }

  p++;
  end = strchr(p, '"');
  if (end == NULL || (size_t)(end - p) >= outlen)
    {
      end = p + outlen - 1;
    }

  memcpy(out, p, end - p);
  out[end - p] = '\0';
}

static bool qpk_probe(const char *dirpath, char *name, size_t namelen)
{
  char path[192];
  size_t dirlen;
  FILE *fp;
  char buf[512];
  size_t n;
  bool ret = false;

  dirlen = strnlen(dirpath, sizeof(path));
  if (dirlen >= sizeof(path) - sizeof("/manifest.json"))
    {
      return false;
    }

  memcpy(path, dirpath, dirlen);
  memcpy(path + dirlen, "/manifest.json", sizeof("/manifest.json"));
  fp = fopen(path, "r");
  if (fp == NULL)
    {
      return false;
    }

  n = fread(buf, 1, sizeof(buf) - 1, fp);
  buf[n] = '\0';
  fclose(fp);

  json_get_string(buf, "\"name\"", name, namelen);
  if (name[0] != '\0')
    {
      ret = true;
    }

  return ret;
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
      if (ent->d_name[0] == '.')
        {
          continue;
        }

      strlcpy(path, QPK_DIR "/", sizeof(path));
      if (strlcat(path, ent->d_name, sizeof(path)) >= sizeof(path))
        {
          continue;
        }
      if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode))
        {
          continue;
        }

      if (!qpk_probe(path, g_desktop.qpk[g_desktop.nqpk].name,
                     NAME_MAXLEN))
        {
          continue;
        }

      strlcpy(g_desktop.qpk[g_desktop.nqpk].dir, ent->d_name,
              sizeof(g_desktop.qpk[g_desktop.nqpk].dir));
      g_desktop.nqpk++;
    }

  closedir(dp);
}

static void panel_hide(lv_event_t *e)
{
  lv_obj_add_flag(g_desktop.panel, LV_OBJ_FLAG_HIDDEN);
}

static void show_panel(const char *title, const char *text)
{
  lv_obj_t *card;
  lv_obj_t *label;
  lv_obj_t *close;

  lv_obj_remove_flag(g_desktop.panel, LV_OBJ_FLAG_HIDDEN);

  lv_obj_clean(g_desktop.panel);

  card = lv_obj_create(g_desktop.panel);
  lv_obj_set_size(card, 640, 400);
  lv_obj_center(card);
  lv_obj_set_style_bg_color(card, lv_color_hex(0x181f33), 0);
  lv_obj_set_style_border_color(card, lv_color_hex(0x39415c), 0);
  lv_obj_set_style_radius(card, 18, 0);

  label = lv_label_create(card);
  lv_label_set_text(label, title);
  lv_obj_set_style_text_color(label, lv_color_hex(0xffffff), 0);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_24, 0);
  lv_obj_align(label, LV_ALIGN_TOP_LEFT, 16, 12);

  label = lv_label_create(card);
  lv_label_set_text(label, text);
  lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(label, 590);
  lv_obj_set_style_text_color(label, lv_color_hex(0xc9d2ee), 0);
  lv_obj_align(label, LV_ALIGN_TOP_LEFT, 16, 56);

  close = lv_button_create(card);
  lv_obj_set_size(close, 96, 44);
  lv_obj_align(close, LV_ALIGN_BOTTOM_RIGHT, -14, -12);
  lv_obj_add_event_cb(close, panel_hide, LV_EVENT_CLICKED, NULL);

  label = lv_label_create(close);
  lv_label_set_text(label, LV_SYMBOL_CLOSE " Close");
  lv_obj_center(label);
}

static void about_clicked(lv_event_t *e)
{
  struct timespec ts;
  char text[256];

  clock_gettime(CLOCK_MONOTONIC, &ts);
  snprintf(text, sizeof(text),
           "ESP32-P4 Function-EV-Board\n"
           "Apache NuttX RTOS\n"
           "CPU: dual-core RV32 @ HP\n"
           "Uptime: %lu s\n"
           "Quick apps: %d",
           (unsigned long)ts.tv_sec, g_desktop.nqpk);

  show_panel("System", text);
}

static void hello_clicked(lv_event_t *e)
{
  show_panel("Welcome", "ESP32-P4 touch desktop is running.\n\n"
                        "Tap an icon to open it. Applications can be "
                        "registered here without changing the display "
                        "or touch drivers.");
}

static void qpk_clicked(lv_event_t *e)
{
  char text[512];
  int i;
  int off = 0;

  if (g_desktop.nqpk == 0)
    {
      show_panel("QuickApp", "No quick apps found.\n"
                             "Install packages under " QPK_DIR ".");
      return;
    }

  text[0] = '\0';
  for (i = 0; i < g_desktop.nqpk && off < (int)sizeof(text) - 64; i++)
    {
      off += snprintf(text + off, sizeof(text) - off,
                      "%s  %s (%s)\n", LV_SYMBOL_FILE,
                      g_desktop.qpk[i].name, g_desktop.qpk[i].dir);
    }

  off += snprintf(text + off, sizeof(text) - off,
                  "\nRun with the qapp runner from NSH.");
  show_panel("QuickApp", text);
}

static void tile_clicked(lv_event_t *e)
{
  intptr_t id = (intptr_t)lv_event_get_user_data(e);
  char text[192];

  switch (id)
    {
      case APP_ABOUT:
        about_clicked(e);
        break;
      case APP_HELLO:
        hello_clicked(e);
        break;
      case APP_QPK:
        qpk_clicked(e);
        break;
      default:
        if (id >= 100 && id < 100 + g_desktop.nqpk)
          {
            int index = id - 100;

            snprintf(text, sizeof(text),
                     "Package: %s\nDirectory: %s/%s\n\n"
                     "The launcher entry is ready. The quick-app runtime "
                     "will be connected in the next stage.",
                     g_desktop.qpk[index].name, QPK_DIR,
                     g_desktop.qpk[index].dir);
            show_panel(g_desktop.qpk[index].name, text);
          }
        break;
    }
}

static void add_tile(lv_obj_t *parent, const char *icon, const char *name,
                     intptr_t id)
{
  lv_obj_t *btn;
  lv_obj_t *label;

  btn = lv_button_create(parent);
  lv_obj_set_size(btn, 150, 150);
  lv_obj_set_style_radius(btn, 20, 0);
  lv_obj_set_style_bg_color(btn, lv_color_hex(0x232b45), 0);
  lv_obj_set_style_shadow_width(btn, 0, 0);
  lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_add_event_cb(btn, tile_clicked, LV_EVENT_CLICKED, (void *)id);

  label = lv_label_create(btn);
  lv_label_set_text(label, icon);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_48, 0);
  lv_obj_set_style_text_color(label, lv_color_hex(0x8ea2ff), 0);

  label = lv_label_create(btn);
  lv_label_set_text(label, name);
  lv_obj_set_style_text_color(label, lv_color_hex(0xffffff), 0);
}

static void clock_timer_cb(lv_timer_t *timer)
{
  struct timespec ts;
  char buf[16];
  time_t sec;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  sec = ts.tv_sec;

  /* No RTC backup battery: show monotonic uptime as mm:ss until the wall
   * clock is set by the user.
   */

  snprintf(buf, sizeof(buf), "up %02ld:%02ld",
           (long)(sec / 60) % 100, (long)sec % 60);
  lv_label_set_text(g_desktop.clock_label, buf);
  LV_UNUSED(timer);
}

static lv_obj_t *statusbar_create(lv_obj_t *parent)
{
  lv_obj_t *bar;
  lv_obj_t *label;

  bar = lv_obj_create(parent);
  lv_obj_set_size(bar, lv_pct(100), 36);
  lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_bg_color(bar, lv_color_hex(0x11162a), 0);
  lv_obj_set_style_border_width(bar, 0, 0);
  lv_obj_set_style_radius(bar, 0, 0);
  lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

  label = lv_label_create(bar);
  lv_label_set_text(label, "NuttX Desktop");
  lv_obj_set_style_text_color(label, lv_color_hex(0x8ea2ff), 0);
  lv_obj_align(label, LV_ALIGN_LEFT_MID, 12, 0);

  label = lv_label_create(bar);
  lv_label_set_text(label, LV_SYMBOL_WIFI "  " LV_SYMBOL_CHARGE);
  lv_obj_set_style_text_color(label, lv_color_hex(0x9aa7cc), 0);
  lv_obj_align(label, LV_ALIGN_RIGHT_MID, -110, 0);

  g_desktop.clock_label = lv_label_create(bar);
  lv_label_set_text(g_desktop.clock_label, "up 00:00");
  lv_obj_set_style_text_color(g_desktop.clock_label,
                              lv_color_hex(0xffffff), 0);
  lv_obj_align(g_desktop.clock_label, LV_ALIGN_RIGHT_MID, -12, 0);

  return bar;
}

static void desktop_ui_create(void)
{
  lv_obj_t *screen;
  lv_obj_t *bar;
  lv_obj_t *grid;
  int i;

  screen = lv_screen_active();
  lv_obj_set_style_bg_color(screen, lv_color_hex(0x0b1020), 0);

  memset(&g_desktop, 0, sizeof(g_desktop));

  bar = statusbar_create(screen);

  grid = lv_obj_create(screen);
  g_desktop.grid = grid;
  lv_obj_set_size(grid, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_align(grid, LV_ALIGN_TOP_MID, 0, 48);
  lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(grid, 0, 0);
  lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_style_pad_column(grid, 22, 0);
  lv_obj_set_style_pad_row(grid, 22, 0);
  lv_obj_set_style_pad_left(grid, 30, 0);
  lv_obj_set_style_pad_top(grid, 10, 0);

  add_tile(grid, g_builtin_icons[APP_ABOUT], g_builtin_names[APP_ABOUT],
           APP_ABOUT);
  add_tile(grid, g_builtin_icons[APP_HELLO], g_builtin_names[APP_HELLO],
           APP_HELLO);
  add_tile(grid, g_builtin_icons[APP_QPK], g_builtin_names[APP_QPK],
           APP_QPK);

  qpk_scan();
  for (i = 0; i < g_desktop.nqpk; i++)
    {
      add_tile(grid, LV_SYMBOL_FILE, g_desktop.qpk[i].name,
               (intptr_t)(100 + i));
    }

  /* Overlay panel used by built-in app pages */

  g_desktop.panel = lv_obj_create(screen);
  lv_obj_set_size(g_desktop.panel, lv_pct(100), lv_pct(100));
  lv_obj_set_style_bg_color(g_desktop.panel, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(g_desktop.panel, LV_OPA_70, 0);
  lv_obj_set_style_border_width(g_desktop.panel, 0, 0);
  lv_obj_add_flag(g_desktop.panel, LV_OBJ_FLAG_HIDDEN);

  lv_timer_create(clock_timer_cb, 1000, NULL);
  LV_UNUSED(bar);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

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

  if (lv_is_initialized())
    {
      return -1;
    }

#ifdef CONFIG_ESP32P4_FUNCTION_EV_BOARD_TOUCHSCREEN
  /* Register /dev/input0 before LVGL starts.  The LVGL NuttX backend owns
   * the nonblocking input fd and its sample/state window.
   */

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
  /* Keep the graphical desktop as the system entry point and run the NSH
   * console independently.  A disconnected or idle console can therefore
   * never hold up LVGL's timer/input loop.
   */

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

  lv_nuttx_deinit(&result);
  lv_deinit();
  return 0;
}
