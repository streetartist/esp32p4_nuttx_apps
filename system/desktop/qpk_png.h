/****************************************************************************
 * apps/system/desktop/qpk_png.h
 *
 * 8-bit RGBA PNG decoder that uses libc malloc (not LVGL's draw-buf cache).
 ****************************************************************************/

#pragma once

#include <stddef.h>
#include <stdint.h>

int qpk_png_decode32(uint8_t **out, unsigned int *width, unsigned int *height,
                     const uint8_t *png, size_t size);
