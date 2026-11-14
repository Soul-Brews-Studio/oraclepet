/*
 * gifplayer.cpp — AnimatedGIF-backed implementation of gifplayer.h.
 *
 * This is the C/C++ bridge for the native pet_sim. It reuses the device-proven
 * decoder path from src/pet.cpp (gifDrawCb + the file callbacks) but:
 *   - uses stdio FILE* callbacks (fopen/fread/fseek/fclose) instead of LittleFS,
 *   - writes RGB565 pixels directly into a caller-owned uint16_t buffer (the
 *     lv_canvas backing store) instead of a LovyanGFX sprite.
 *
 * The destination buffer + geometry are passed to the GIFDRAW callback via
 * AnimatedGIF's user pointer: gp_play_frame() hands a DrawCtx* to
 * gif.playFrame(false, &delay, &ctx), and AnimatedGIF propagates it to
 * GIFDRAW->pUser (see AnimatedGIF.cpp playFrame -> _gif.pUser -> gd.pUser).
 * No file-scope statics are needed for the draw target, which keeps it
 * re-entrant-friendly and matches the gd.pUser plumbing in gif.inl.
 *
 * ENDIANNESS NOTE: we begin() with GIF_PALETTE_RGB565_LE (== LITTLE_ENDIAN_PIXELS,
 * what pet.cpp uses) and write pPalette[idx] straight through as the uint16_t
 * pixel value. lv_canvas with LV_COLOR_FORMAT_RGB565 stores native-endian
 * uint16_t, so on a little-endian host this matches. If colors look
 * byte-swapped (e.g. blue<->red bias), flip this to GIF_PALETTE_RGB565_BE.
 */
#include "gifplayer.h"

#include <AnimatedGIF.h>
#include <stdio.h>

/* Decoder + currently-open file handle (one GIF at a time, like pet.cpp). */
static AnimatedGIF s_gif;
static FILE       *s_fp      = nullptr;
static bool        s_open    = false;
static bool        s_begun   = false;
static int         s_w       = 0;   /* native GIF canvas width  */
static int         s_h       = 0;   /* native GIF canvas height */

/* End-of-animation latch: AnimatedGIF::playFrame returns 1 while more frames
 * exist and 0 on the LAST frame (which still draws). We want gp_play_frame to
 * render that last frame WITH its delay, then signal 0 on the following call so
 * the caller (main_pet.c) can gp_rewind() and loop. This latch carries that. */
static bool        s_atEnd   = false;

/* Per-call draw context handed to the GIFDRAW callback via GIFDRAW->pUser. */
struct DrawCtx {
    uint16_t *dst;        /* RGB565 destination buffer        */
    int       dstW, dstH; /* destination dimensions           */
    int       scale;      /* nearest-neighbor upscale factor  */
    uint16_t  bg;         /* background / transparent fill    */
    int       gifX, gifY; /* top-left of upscaled art in dst  */
};

/* ── stdio file callbacks (mirror pet.cpp's LittleFS callbacks) ───────────── */

static void *gifOpenCb(const char *fname, int32_t *pSize) {
    FILE *f = fopen(fname, "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END);
    *pSize = (int32_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    return (void *)f;
}

static void gifCloseCb(void *handle) {
    FILE *f = (FILE *)handle;
    if (f) fclose(f);
}

static int32_t gifReadCb(GIFFILE *pFile, uint8_t *pBuf, int32_t iLen) {
    FILE *f = (FILE *)pFile->fHandle;
    int32_t n = (int32_t)fread(pBuf, 1, (size_t)iLen, f);
    pFile->iPos = (int32_t)ftell(f);
    return n;
}

static int32_t gifSeekCb(GIFFILE *pFile, int32_t iPosition) {
    FILE *f = (FILE *)pFile->fHandle;
    fseek(f, iPosition, SEEK_SET);
    pFile->iPos = (int32_t)ftell(f);
    return pFile->iPos;
}

/* ── Draw callback ────────────────────────────────────────────────────────
 * One decoded scanline -> scale×scale RGB565 blocks (nearest-neighbor upscale),
 * transparent palette index -> bg. Ported from pet.cpp::gifDrawCb, but writes
 * directly into the DrawCtx::dst buffer (clipped to dst bounds) instead of
 * calling spr.fillRect. */
static void gifDrawCb(GIFDRAW *d) {
    DrawCtx *c = (DrawCtx *)d->pUser;
    if (!c || !c->dst) return;

    const uint16_t *pal16 = d->pPalette;
    const uint8_t  *src   = d->pPixels;
    const uint8_t   t     = d->ucTransparent;
    const bool      hasT  = d->ucHasTransparency != 0;
    const int       srcY  = d->iY + d->y;
    const int       S     = c->scale;

    int w = d->iWidth;
    if (w > 256) w = 256;              /* same clamp pet.cpp uses */

    const int oy = c->gifY + srcY * S; /* dst y of this scanline's block top */
    if (oy + S <= 0 || oy >= c->dstH) return;   /* whole row off-screen */

    for (int i = 0; i < w; i++) {
        const uint8_t  idx   = src[i];
        const uint16_t color = (hasT && idx == t) ? c->bg : pal16[idx];
        const int ox  = c->gifX + (d->iX + i) * S;

        int cx0 = ox < 0 ? 0 : ox;
        int cy0 = oy < 0 ? 0 : oy;
        int cx1 = ox + S; if (cx1 > c->dstW) cx1 = c->dstW;
        int cy1 = oy + S; if (cy1 > c->dstH) cy1 = c->dstH;
        if (cx0 >= cx1 || cy0 >= cy1) continue;

        for (int yy = cy0; yy < cy1; yy++) {
            uint16_t *row = c->dst + (size_t)yy * c->dstW;
            for (int xx = cx0; xx < cx1; xx++) row[xx] = color;
        }
    }
}

/* ── gifPlace() — center the upscaled GIF (ported from pet.cpp::gifPlace) ──
 * Centers in the FULL dst here: the caller sizes dst to the usable pet area
 * (320x400, the HUD lives outside this canvas), so centering in dst == centering
 * above the HUD. Clamp origin >= 0 so tall art anchors top-left. */
static void gifPlace(DrawCtx *c) {
    const int outW = s_w * c->scale;
    const int outH = s_h * c->scale;
    c->gifX = (c->dstW - outW) / 2;
    c->gifY = (c->dstH - outH) / 2;
    if (c->gifX < 0) c->gifX = 0;
    if (c->gifY < 0) c->gifY = 0;
}

/* ── Public API ───────────────────────────────────────────────────────────── */

extern "C" int gp_open(const char *path, int *w, int *h) {
    if (s_open) { s_gif.close(); s_open = false; }
    s_fp = nullptr;

    if (!s_begun) {
        /* GIF_PALETTE_RGB565_LE == LITTLE_ENDIAN_PIXELS — same as pet.cpp.
         * Flip to GIF_PALETTE_RGB565_BE if colors look byte-swapped. */
        s_gif.begin(GIF_PALETTE_RGB565_LE);
        s_begun = true;
    }

    if (!s_gif.open(path, gifOpenCb, gifCloseCb, gifReadCb, gifSeekCb, gifDrawCb)) {
        fprintf(stderr, "[gifplayer] open failed: %s (err %d)\n",
                path, s_gif.getLastError());
        return 1;
    }
    s_open  = true;
    s_atEnd = false;
    s_w     = s_gif.getCanvasWidth();
    s_h     = s_gif.getCanvasHeight();
    if (w) *w = s_w;
    if (h) *h = s_h;
    return 0;
}

extern "C" int gp_play_frame(uint16_t *dst, int dstW, int dstH, int scale, uint16_t bg) {
    if (!s_open || !dst) return -1;

    /* Clear to bg first — GIFs here are unoptimized full-frame, so each frame
     * fully repaints its region; clearing avoids stale pixels from prior art. */
    const size_t n = (size_t)dstW * (size_t)dstH;
    for (size_t i = 0; i < n; i++) dst[i] = bg;

    /* If the previous call drew the final frame, signal end-of-animation now
     * (caller gp_rewind()s + replays). */
    if (s_atEnd) {
        s_atEnd = false;
        return 0;
    }

    DrawCtx ctx;
    ctx.dst   = dst;
    ctx.dstW  = dstW;
    ctx.dstH  = dstH;
    ctx.scale = scale;
    ctx.bg    = bg;
    gifPlace(&ctx);   /* uses s_w/s_h captured at gp_open */

    int delay = 0;
    int rc = s_gif.playFrame(false, &delay, &ctx);
    if (rc < 0) {
        fprintf(stderr, "[gifplayer] playFrame error (err %d)\n", s_gif.getLastError());
        return -1;
    }
    if (rc == 0) {
        /* This was the last frame — it WAS drawn into dst. Render it now with a
         * sane delay; latch end so the next call returns 0. */
        s_atEnd = true;
    }
    return delay > 0 ? delay : 100;
}

extern "C" void gp_rewind(void) {
    if (!s_open) return;
    s_gif.reset();   /* seeks the file back to 0 */
    s_atEnd = false;
}

extern "C" void gp_close(void) {
    if (s_open) { s_gif.close(); s_open = false; }
    s_fp    = nullptr;
    s_atEnd = false;
}
