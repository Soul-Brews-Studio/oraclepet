/**
 * @file lv_conf.h
 * LVGL v9 configuration for the jc3248-pet desktop simulator (pet_sim).
 *
 * Copied from lab/jc3248w535/sim/lv_conf.h (the proven oracle-face harness) and
 * extended for the pet:
 *   - LV_USE_GIF 1            — decode the bufo animated GIFs
 *   - LV_USE_FS_STDIO 1       — read GIFs off disk via "S:" paths (stdio fopen)
 *   - LV_MEM_SIZE bumped to 2 MB — gif decode needs heap headroom on desktop
 *   - LV_FONT_MONTSERRAT_28   — large pack name in the HUD
 * Keeps LV_COLOR_DEPTH 16 + LV_USE_SDL 1 like the original.
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
/* GIF frames are decoded full-canvas into RAM; 256 KB (the face sim's heap) is
   too small once a few packs are open. Give the desktop sim 2 MB. */
#define LV_MEM_SIZE (2U * 1024U * 1024U)

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
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_22 0
/* Large pack name ("bufo") in the HUD. */
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

#define LV_USE_FONT_COMPRESSED 1

/*====================
   WIDGETS
 *====================*/
#define LV_USE_LABEL 1
#define LV_LABEL_TEXT_SELECTION 0
#define LV_LABEL_LONG_TXT_HINT 1
#define LV_USE_OBJ 1
#define LV_USE_BUTTON 1
#define LV_USE_IMAGE 1
/* The pet is an lv_canvas: the AnimatedGIF decoder (gifplayer.cpp) blits RGB565
   frames straight into its backing buffer. */
#define LV_USE_CANVAS 1

/*====================
   LIBS — GIF decoder
 *====================*/
/* DISABLED: lvgl's bundled gifdec overflows/segfaults on bufo's GIFs. We now
   decode with AnimatedGIF (bitbank2) in gifplayer.cpp -> lv_canvas instead. */
#define LV_USE_GIF 0

/*====================
   FILESYSTEM — STDIO (lets lv_gif_set_src read "S:" paths off disk)
 *====================*/
/* The firmware reads GIFs from LittleFS; the desktop sim reads them straight
   from the host filesystem via fopen(). Drive letter 'S', empty path prefix so
   "S:" maps to the raw path we build (an absolute or build-dir-relative path). */
#define LV_USE_FS_STDIO 1
#define LV_FS_STDIO_LETTER 'S'
#define LV_FS_STDIO_PATH ""
#define LV_FS_STDIO_CACHE_SIZE 0

/*====================
   SDL DRIVER  (renders the window on Mac)
 *====================*/
#define LV_USE_SDL 1
#define LV_SDL_INCLUDE_PATH <SDL.h>
#define LV_SDL_RENDER_MODE LV_DISPLAY_RENDER_MODE_DIRECT
#define LV_SDL_BUF_COUNT 1
#define LV_SDL_FULLSCREEN 0
#define LV_SDL_DIRECT_EXIT 1

#endif /* LV_CONF_H */
