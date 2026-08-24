/****************************************************************************
 * apps/system/desktop/desktop_font.c
 *
 * Embed the complete Alibaba PuHuiTi OpenType font in read-only flash.
 ****************************************************************************/

#include <nuttx/config.h>

__asm__(
  ".pushsection .rodata.desktop_font,\"a\",@progbits\n"
  ".balign 4\n"
  ".global g_desktop_font_start\n"
  "g_desktop_font_start:\n"
  ".incbin \"AlibabaPuHuiTi-3-55-Regular.otf\"\n"
  ".global g_desktop_font_end\n"
  "g_desktop_font_end:\n"
  ".balign 4\n"
  ".popsection\n");

