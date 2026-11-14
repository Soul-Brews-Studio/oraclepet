/**
 * @file lv_conf.h
 * Minimal LVGL v9 configuration for the oracle-face desktop simulator.
 * Mirrors what we'd actually use on the AXS15231 320x480 panel.
 */

#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/*====================
   COLOR / RESOLUTION
 *====================*/
#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 0

/*====================
   MEMORY
 *====================*/
#define LV_USE_STDLIB_MALLOC LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_STRING LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_BUILTIN
#define LV_MEM_SIZE (256U * 1024U)

/*====================
   TIMING / OS
 *====================*/
#define LV_USE_OS LV_OS_NONE
#define LV_DEF_REFR_PERIOD 16
#define LV_TICK_CUSTOM 0

/*====================
   DRAW
 *====================*/
#define LV_DRAW_BUF_STRIDE_ALIGN 1
#define LV_DRAW_BUF_ALIGN 4
#define LV_USE_DRAW_SW 1
#define LV_DRAW_SW_SUPPORT_RGB565 1
#define LV_DRAW_SW_SUPPORT_RGB888 1
#define LV_DRAW_SW_SUPPORT_XRGB8888 1
#define LV_DRAW_SW_SUPPORT_ARGB8888 1

/*====================
   LOGGING
 *====================*/
#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 1

/*====================
   FONTS
 *====================*/
/* Built-in Montserrat doesn't carry Greek (no ψ). We ship Noto Sans subsets
   in fonts_gen/ that include U+03C8. Keep one built-in font as fallback so
   LV_FONT_DEFAULT has something to point to before main() wires noto_*. */
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_UNSCII_8 1
#define LV_FONT_UNSCII_16 1
#define LV_FONT_MONTSERRAT_22 0
#define LV_FONT_MONTSERRAT_28 0
#define LV_FONT_DEFAULT &lv_font_montserrat_14

/* lv_font_conv emits compressed glyph bitmaps by default; let LVGL decode them. */
#define LV_USE_FONT_COMPRESSED 1

/*====================
   WIDGETS
 *====================*/
#define LV_USE_LABEL 1
#define LV_LABEL_TEXT_SELECTION 0
#define LV_LABEL_LONG_TXT_HINT 1
#define LV_USE_OBJ 1

/*====================
   SDL DRIVER  (the part that lets us render on Mac)
 *====================*/
#define LV_USE_SDL 1
#define LV_SDL_INCLUDE_PATH <SDL.h>
#define LV_SDL_RENDER_MODE LV_DISPLAY_RENDER_MODE_DIRECT
#define LV_SDL_BUF_COUNT 1
#define LV_SDL_FULLSCREEN 0
#define LV_SDL_DIRECT_EXIT 1

#endif /* LV_CONF_H */
