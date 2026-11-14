/*
 * gifplayer.h — C-callable GIF player for the pet_sim, backed by AnimatedGIF
 * (bitbank2). The ONLY C/C++ boundary in the sim: main_pet.c (C) talks to
 * gifplayer.cpp (C++ + AnimatedGIF) through this header.
 *
 * Mirrors the device-proven decoder path in src/pet.cpp: open a GIF via stdio
 * FILE* callbacks, decode one frame at a time, 3x nearest-neighbor upscale by
 * writing scale×scale RGB565 blocks, transparent palette index -> bg, and
 * center the upscaled art (gifPlace() math) in the destination buffer.
 *
 * Unlike the firmware (LovyanGFX sprite + LittleFS), this writes RGB565 pixels
 * directly into a caller-owned uint16_t buffer (the lv_canvas backing store) and
 * uses stdio (fopen/fread/fseek/fclose) — NOT LittleFS.
 */
#ifndef GIFPLAYER_H
#define GIFPLAYER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Open a GIF file for playback. `path` is a plain filesystem path (no "S:"
 * prefix — that's an LVGL drive-letter convention; gifplayer uses stdio).
 * On success returns 0 and fills w and h with the GIF's native canvas size.
 * Returns nonzero on failure (file not found / not a GIF). Any previously
 * open GIF is closed first. */
int gp_open(const char *path, int *w, int *h);

/* Clear `dst` (dstW x dstH RGB565) to `bg`, decode the NEXT frame, and write it
 * as scale×scale nearest-neighbor blocks, centered via gifPlace() math, with
 * transparent palette index -> bg. Output is clipped to the dst bounds.
 * Returns:
 *    >0  frame delay in milliseconds (reschedule the next call after this)
 *     0  end of animation (this was the last frame; caller should gp_rewind()
 *        to loop)
 *    -1  error (no GIF open / decode failure)
 */
int gp_play_frame(uint16_t *dst, int dstW, int dstH, int scale, uint16_t bg);

/* Reset playback to the first frame (for looping single-gif states). */
void gp_rewind(void);

/* Close the current GIF and release its file handle. Safe to call when none
 * is open. */
void gp_close(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* GIFPLAYER_H */
