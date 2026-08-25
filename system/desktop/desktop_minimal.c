/****************************************************************************
 * apps/system/desktop/desktop_minimal.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal LVGL framebuffer bring-up screen.
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>
#include <unistd.h>

#include <lvgl/lvgl.h>

int main(int argc, FAR char *argv[])
{
  lv_nuttx_dsc_t info;
  lv_nuttx_result_t result;
  FAR lv_obj_t *screen;

  if (lv_is_initialized())
    {
      return 1;
    }

  lv_init();
  lv_nuttx_dsc_init(&info);
  info.fb_path = "/dev/fb0";
#ifdef CONFIG_INPUT_TOUCHSCREEN
  info.input_path = NULL;
#endif

  lv_nuttx_init(&info, &result);
  if (result.disp == NULL)
    {
      return 1;
    }

  screen = lv_screen_active();
  lv_obj_set_style_bg_color(screen, lv_color_hex(0x00ff00), 0);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
  lv_refr_now(result.disp);

  for (;;)
    {
      uint32_t idle = lv_timer_handler();
      usleep((idle ? idle : 1) * 1000);
    }

  return 0;
}
