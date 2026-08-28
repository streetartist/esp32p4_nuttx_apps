/****************************************************************************
 * apps/system/desktop/desktop_camera.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __APPS_SYSTEM_DESKTOP_DESKTOP_CAMERA_H
#define __APPS_SYSTEM_DESKTOP_DESKTOP_CAMERA_H

#include <lvgl/lvgl.h>

int desktop_camera_start(lv_obj_t *parent, const lv_font_t *font);
void desktop_camera_stop(void);

#endif /* __APPS_SYSTEM_DESKTOP_DESKTOP_CAMERA_H */
