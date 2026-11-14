/*
 * gif.cpp — jc3248-pet-idf STEP 6: animated-GIF renderer (the headline).
 *
 * A near-1:1 port of the proven Arduino renderer lab/jc3248-pet/src/pet.cpp,
 * with three substitutions for the ESP-IDF body:
 *   - LovyanGFX sprite  ->  display.c's existing PSRAM framebuffer (display_framebuffer()).
 *                           The gif draw callback writes RGB565 straight into s_fb;
 *                           display_flush() pushes it via esp_lcd_panel_draw_bitmap.
 *                           NO second framebuffer is allocated here.
 *   - Arduino LittleFS  ->  POSIX stdio (fopen/fread/fseek) on the VFS mount that
 *                           step 5 registers at GIF_MOUNT ("/littlefs"). The
 *                           AnimatedGIF file callbacks wrap FILE*.
 *   - ArduinoJson       ->  a tiny purpose-built manifest scanner (manifest_parse).
 *                           No JSON component dependency; falls back to scanning the
 *                           pack dir for idle_*.gif if the manifest is missing.
 *   - millis()          ->  esp_timer_get_time()/1000.
 *
 * AnimatedGIF API used (vendored component, C++ class — see components/AnimatedGIF):
 *   gif.begin(LITTLE_ENDIAN_PIXELS)   palette type GIF_PALETTE_RGB565_LE — palette
 *                                     entries land in s_fb in the SAME host/LE byte
 *                                     order display.c's test pattern uses (e.g. red
 *                                     == 0xF800), so esp_lcd renders them correctly.
 *   gif.open(path, openCb, closeCb, readCb, seekCb, drawCb)  -> nonzero on success
 *   gif.getCanvasWidth() / getCanvasHeight()  -> logical frame size (pre-upscale)
 *   gif.playFrame(false, &delayMs)    -> 1 = more frames, 0 = last frame played,
 *                                        -1 = error; delayMs = this frame's delay
 *   gif.reset()                       -> seek back to first frame (loop a single GIF)
 *   gif.close()                       -> close via the close callback
 *   gif.getLastError()                -> GIF_* code for logging
 * The decode runs the portable C cookPixels/mergeTransparent path (the SIMD .S is
 * not vendored / NO_SIMD is set — see the component CMakeLists), so no esp-dsp dep.
 *
 * Scope (v1 headline): idle_0..N rotation with a dwell + tap reaction. The BLE
 * activity states (busy/attention/sleep) and the HUD are deliberately left for
 * later steps; the manifest fields for them are simply ignored here.
 */
#include "gif.h"
#include "display.h"
#include "hud_gfx.h"     /* gfx_text — for the always-on "Cat Lab Brewing" signature */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <dirent.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_timer.h"

#include <AnimatedGIF.h>

static const char *TAG = "gif";

/* Panel geometry — literal, same as pet.cpp (do NOT pull display.c statics). */
#define PET_W      DISP_W           /* 320 */
#define PET_H      DISP_H           /* 480 */
#define PET_HUD_H  80               /* reserved strip at bottom; pet centers above */
#define GIF_SCALE  3

#define MAX_IDLE       16
#define ANIM_PAUSE_MS  3000         /* dwell before rotating to the next idle GIF */
#define REACT_MS       4000         /* how long a tap reaction loops */

/* ── Always-on signature line ────────────────────────────────────────────────
 * A persistent "Cat Lab Brewing" watermark drawn BELOW the frog and just above
 * the HUD strip, every frame. It must be redrawn after each compose because
 * display_fb_clear() wipes the whole framebuffer per frog frame (the HUD's
 * bottom strip [HUD_Y0,PET_H) is repainted by hud_draw(), but this gap is not).
 * Dim gray (0xC618) matches the HUD's secondary text; gfx_text() byte-swaps
 * internally for the big-endian panel, so we pass a host-order colour. Drawn
 * LAST so it stays legible even if a tall reaction GIF reaches into the gap. */
#define SIG_TEXT   "Cat Lab Brewing"
#define SIG_SCALE  2                              /* 16px tall, 8*2 px per glyph */
#define SIG_COLOR  0xC618                          /* light gray (= HUD_TEXT_DIM) */
#define SIG_Y      (PET_H - PET_HUD_H - 16 - 4)    /* 380: 4px above the HUD strip */

/* ── millis() shim ─────────────────────────────────────────────────────────*/
static inline uint32_t millis(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ── Loaded pack state (mirrors pet.cpp file-scope statics) ─────────────────*/
static bool     s_loaded = false;
static char     s_basePath[80];                 /* GIF_BASE_DIR "/<name>" */
static char     s_packName[24] = { 0 };         /* "<name>" alone — shown in the HUD */
static char     s_idlePaths[MAX_IDLE][32];      /* idle GIF filenames (no dir) */
static uint8_t  s_idleCount = 0;
static uint8_t  s_idleRot   = 0;
static char     s_reactPath[32] = { 0 };        /* heart/celebrate; "" = none */
static uint16_t s_bg = 0x0000;                  /* RGB565 background (from manifest) */

/* The framebuffer the gif draw callback paints into — display.c owns it. */
static uint16_t *s_fb = NULL;

static void draw_signature(void);       /* always-on "Cat Lab Brewing" line (defined near gif_tick) */

/* ── AnimatedGIF runtime state ──────────────────────────────────────────────*/
static AnimatedGIF s_gif;
static FILE       *s_file        = NULL;
static int         s_gifX = 0, s_gifY = 0, s_gifW = 0, s_gifH = 0;
static bool        s_gifOpen     = false;
static uint32_t    s_nextFrameAt = 0;
static uint32_t    s_animPause   = 0;           /* dwell-until between idle GIFs */

/* Tap reaction. */
static bool        s_reacting    = false;
static uint32_t    s_reactUntil  = 0;

/* ── #RRGGBB / RRGGBB -> RGB565 (ported parseHexColor from pet.cpp) ─────────*/
static uint16_t parseHexColor(const char *s, uint16_t fallback)
{
    if (!s) return fallback;
    if (*s == '#') s++;
    uint32_t v = strtoul(s, NULL, 16);
    return (uint16_t)(((v >> 19) & 0x1F) << 11 | ((v >> 10) & 0x3F) << 5 | ((v >> 3) & 0x1F));
}

/* ── AnimatedGIF file callbacks over POSIX FILE* (replace Arduino LittleFS) ──*/
static void *gifOpenCb(const char *fname, int32_t *pSize)
{
    FILE *f = fopen(fname, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *pSize = (int32_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    s_file = f;
    return (void *)f;
}
static void gifCloseCb(void *handle)
{
    FILE *f = (FILE *)handle;
    if (f) fclose(f);
    if (f == s_file) s_file = NULL;
}
static int32_t gifReadCb(GIFFILE *pFile, uint8_t *pBuf, int32_t iLen)
{
    FILE *f = (FILE *)pFile->fHandle;
    int32_t n = (int32_t)fread(pBuf, 1, (size_t)iLen, f);
    pFile->iPos = (int32_t)ftell(f);
    return n;
}
static int32_t gifSeekCb(GIFFILE *pFile, int32_t iPosition)
{
    FILE *f = (FILE *)pFile->fHandle;
    fseek(f, iPosition, SEEK_SET);
    pFile->iPos = (int32_t)ftell(f);
    return pFile->iPos;
}

/* ── Frame placement: center the 3x-upscaled GIF above the HUD strip ────────
 * Ported from pet.cpp::gifPlace (home mode). Clamp origin >= 0.
 */
static void gifPlace(void)
{
    int outW = s_gifW * GIF_SCALE;
    int outH = s_gifH * GIF_SCALE;
    s_gifX = (PET_W - outW) / 2;
    int usableH = PET_H - PET_HUD_H;
    s_gifY = (usableH - outH) / 2;
    if (s_gifX < 0) s_gifX = 0;
    if (s_gifY < 0) s_gifY = 0;
}

/* ── Draw callback: one decoded scanline -> GIF_SCALE x GIF_SCALE blocks ─────
 * Nearest-neighbor 3x upscale, written straight into s_fb. Transparent palette
 * index -> s_bg (frames are full-frame, so each repaints its own region). Ported
 * from pet.cpp::gifDrawCb (home-mode branch). The palette is RGB565_LE so a value
 * written into s_fb matches display.c's host-order convention (red == 0xF800).
 */
static void gifDrawCb(GIFDRAW *d)
{
    if (!s_fb) return;
    uint16_t *pal16 = d->pPalette;
    uint8_t  *src   = d->pPixels;
    uint8_t   t     = d->ucTransparent;
    bool      hasT  = d->ucHasTransparency;
    int       srcY  = d->iY + d->y;

    int w = d->iWidth;
    if (w > 256) w = 256;
    int oy = s_gifY + srcY * GIF_SCALE;
    if (oy + GIF_SCALE <= 0 || oy >= PET_H) return;

    for (int i = 0; i < w; i++) {
        uint8_t  idx   = src[i];
        uint16_t color = (hasT && idx == t) ? s_bg : pal16[idx];
        int ox  = s_gifX + (d->iX + i) * GIF_SCALE;
        int cx0 = ox < 0 ? 0 : ox;
        int cy0 = oy < 0 ? 0 : oy;
        int cx1 = ox + GIF_SCALE; if (cx1 > PET_W) cx1 = PET_W;
        int cy1 = oy + GIF_SCALE; if (cy1 > PET_H) cy1 = PET_H;
        for (int yy = cy0; yy < cy1; yy++) {
            uint16_t *row = &s_fb[yy * PET_W];
            for (int xx = cx0; xx < cx1; xx++) row[xx] = color;
        }
    }
}

/* ── Open a GIF by filename (relative to s_basePath) ────────────────────────
 * Mirrors pet.cpp::openGif: sets gifW/gifH, places it, arms timing.
 */
static bool openGif(const char *fname)
{
    if (s_gifOpen) { s_gif.close(); s_gifOpen = false; }

    char full[128];
    snprintf(full, sizeof(full), "%.95s/%.31s", s_basePath, fname);
    if (!s_gif.open(full, gifOpenCb, gifCloseCb, gifReadCb, gifSeekCb, gifDrawCb)) {
        ESP_LOGE(TAG, "open failed: %s (err %d)", full, s_gif.getLastError());
        return false;
    }
    s_gifOpen = true;
    s_gifW = s_gif.getCanvasWidth();
    s_gifH = s_gif.getCanvasHeight();
    gifPlace();
    s_nextFrameAt = 0;
    ESP_LOGI(TAG, "%s: %dx%d @ (%d,%d)", fname, s_gifW, s_gifH, s_gifX, s_gifY);
    return true;
}
static bool openIdle(uint8_t rot)
{
    if (s_idleCount == 0) return false;
    return openGif(s_idlePaths[rot]);
}

/* ── Tiny manifest scanner ──────────────────────────────────────────────────
 * The bufo manifest.json is small and well-formed; rather than pull a JSON
 * component we extract exactly what the v1 renderer needs:
 *   colors.bg              -> s_bg
 *   states.idle (array OR a single string) -> s_idlePaths / s_idleCount
 *   states.heart || states.celebrate       -> s_reactPath
 * Returns true if at least one idle GIF was found. On any failure the caller
 * falls back to scanning the directory for idle_*.gif.
 */
static const char *find_key(const char *buf, const char *key)
{
    /* find "key" then the ':' after it (cheap, no nesting awareness needed for
     * these flat top-level-ish keys in this manifest). */
    char pat[40];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(buf, pat);
    if (!p) return NULL;
    p = strchr(p + strlen(pat), ':');
    return p ? p + 1 : NULL;
}
/* copy the next JSON string literal starting at/after p into out; returns end. */
static const char *copy_string(const char *p, char *out, size_t n)
{
    while (*p && *p != '"') p++;
    if (*p != '"') { if (n) out[0] = 0; return p; }
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < n) out[i++] = *p++;
    out[i] = 0;
    if (*p == '"') p++;
    return p;
}
static bool manifest_parse(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 8192) { fclose(f); return false; }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return false; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = 0;

    /* colors.bg */
    const char *bgp = find_key(buf, "bg");
    if (bgp) { char c[12]; copy_string(bgp, c, sizeof(c)); s_bg = parseHexColor(c, s_bg); }

    /* states.idle: array of strings OR one string. Find "idle"'s value start. */
    s_idleCount = 0;
    const char *idle = find_key(buf, "idle");
    if (idle) {
        const char *p = idle;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (*p == '[') {
            p++;
            while (s_idleCount < MAX_IDLE) {
                /* skip to next string or end of array */
                while (*p && *p != '"' && *p != ']') p++;
                if (*p != '"') break;
                char fn[32];
                p = copy_string(p, fn, sizeof(fn));
                if (fn[0]) { snprintf(s_idlePaths[s_idleCount], 32, "%s", fn); s_idleCount++; }
            }
        } else {
            char fn[32];
            copy_string(p, fn, sizeof(fn));
            if (fn[0]) { snprintf(s_idlePaths[0], 32, "%s", fn); s_idleCount = 1; }
        }
    }

    /* reaction: prefer heart, else celebrate. */
    s_reactPath[0] = 0;
    const char *rp = find_key(buf, "heart");
    if (!rp) rp = find_key(buf, "celebrate");
    if (rp) copy_string(rp, s_reactPath, sizeof(s_reactPath));

    free(buf);
    return s_idleCount > 0;
}

/* Fallback: scan the pack dir for idle_*.gif when there's no usable manifest. */
static bool scan_idle_dir(void)
{
    DIR *dir = opendir(s_basePath);
    if (!dir) return false;
    s_idleCount = 0;
    struct dirent *e;
    while ((e = readdir(dir)) && s_idleCount < MAX_IDLE) {
        if (strncmp(e->d_name, "idle_", 5) == 0 && strstr(e->d_name, ".gif")) {
            snprintf(s_idlePaths[s_idleCount], 32, "%.31s", e->d_name);
            s_idleCount++;
        }
    }
    closedir(dir);
    /* idle_0..N order matters for rotation; readdir order isn't guaranteed, so
     * sort the small list lexically (idle_0 < idle_1 < ... < idle_9). */
    for (int i = 0; i < s_idleCount; i++)
        for (int j = i + 1; j < s_idleCount; j++)
            if (strcmp(s_idlePaths[j], s_idlePaths[i]) < 0) {
                char tmp[32];
                strcpy(tmp, s_idlePaths[i]);
                strcpy(s_idlePaths[i], s_idlePaths[j]);
                strcpy(s_idlePaths[j], tmp);
            }
    return s_idleCount > 0;
}

/* Find the first character pack directory under GIF_BASE_DIR. */
static bool find_first_pack(char *nameOut, size_t n)
{
    DIR *dir = opendir(GIF_BASE_DIR);
    if (!dir) { ESP_LOGE(TAG, "opendir %s failed", GIF_BASE_DIR); return false; }
    struct dirent *e;
    bool found = false;
    while ((e = readdir(dir))) {
        if (e->d_name[0] == '.') continue;
        char full[300];
        snprintf(full, sizeof(full), "%s/%.255s", GIF_BASE_DIR, e->d_name);
        struct stat st;
        if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
            snprintf(nameOut, n, "%s", e->d_name);
            found = true;
            break;
        }
    }
    closedir(dir);
    return found;
}

/* ── Public: setup ──────────────────────────────────────────────────────────*/
esp_err_t gif_setup(void)
{
    s_fb = display_framebuffer();
    if (!s_fb) {
        ESP_LOGE(TAG, "display framebuffer is NULL — call display_init() first");
        return ESP_ERR_INVALID_STATE;
    }

    char name[24] = { 0 };
#ifdef PET_DEFAULT_PACK
    /* Build-time default pack (multiple-firmware: one .bin per pack, all packs
     * baked in the LittleFS image). Use it if its dir exists; else fall back to
     * the first pack found — so a build with no/absent PET_DEFAULT_PACK still
     * behaves as before. */
    {
        char full[300];
        struct stat st;
        snprintf(full, sizeof(full), "%s/%s", GIF_BASE_DIR, PET_DEFAULT_PACK);
        if (stat(full, &st) == 0 && S_ISDIR(st.st_mode))
            snprintf(name, sizeof(name), "%s", PET_DEFAULT_PACK);
        else
            ESP_LOGW(TAG, "PET_DEFAULT_PACK '%s' not found — using first pack", PET_DEFAULT_PACK);
    }
#endif
    if (name[0] == '\0' && !find_first_pack(name, sizeof(name))) {
        ESP_LOGE(TAG, "no character pack under %s", GIF_BASE_DIR);
        return ESP_ERR_NOT_FOUND;
    }
    snprintf(s_basePath, sizeof(s_basePath), "%s/%.23s", GIF_BASE_DIR, name);
    snprintf(s_packName, sizeof(s_packName), "%.23s", name);   /* HUD line 1 */
    ESP_LOGI(TAG, "pack: %s", s_basePath);

    char mpath[128];
    snprintf(mpath, sizeof(mpath), "%.95s/manifest.json", s_basePath);
    if (!manifest_parse(mpath)) {
        ESP_LOGW(TAG, "manifest parse failed/empty (%s) — scanning for idle_*.gif", mpath);
        if (!scan_idle_dir()) {
            ESP_LOGE(TAG, "no idle GIFs in %s", s_basePath);
            return ESP_ERR_NOT_FOUND;
        }
    }
    ESP_LOGI(TAG, "idle GIFs: %u, reaction: %s, bg=0x%04X",
             (unsigned)s_idleCount, s_reactPath[0] ? s_reactPath : "-", s_bg);

    s_gif.begin(BIG_ENDIAN_PIXELS);      /* RGB565_BE — _LE rendered colour-inverted on this panel */
    s_loaded = true;

    /* Decode frame 0 of the first idle GIF and push it. */
    s_idleRot = 0;
    display_fb_clear(s_bg);
    if (openIdle(s_idleRot)) {
        int delayMs = 0;
        s_gif.playFrame(false, &delayMs);
        s_nextFrameAt = millis() + (delayMs > 0 ? (uint32_t)delayMs : 100);
        draw_signature();                /* stamp the signature onto frame 0 */
        display_flush();
        ESP_LOGI(TAG, "frame 0 on screen");
        return ESP_OK;
    }
    ESP_LOGE(TAG, "failed to open first idle GIF");
    s_loaded = false;
    return ESP_FAIL;
}

/* ── Public: tap reaction ───────────────────────────────────────────────────*/
void gif_on_touch(void)
{
    if (!s_loaded || s_reactPath[0] == 0) return;
    s_reactUntil = millis() + REACT_MS;
    if (s_reacting) return;              /* already reacting — just extended */
    s_reacting  = true;
    s_animPause = 0;
    display_fb_clear(s_bg);
    openGif(s_reactPath);                /* nextFrameAt=0 -> next tick draws frame 0 */
    ESP_LOGI(TAG, "touch -> reaction '%s'", s_reactPath);
}

/* ── Public: accessors for the HUD (step 7) ─────────────────────────────────*/
uint16_t gif_bg(void)        { return s_bg; }
const char *gif_pack_name(void) { return s_packName; }

/* ── Internal: advance one frame; returns true iff s_fb changed ─────────────
 * Ported from pet.cpp::tick_frame, reduced to idle-rotation + tap-reaction
 * (no BLE-driven busy/attention/sleep states in v1).
 */
static bool tick_frame(void)
{
    if (!s_loaded) return false;
    uint32_t now = millis();

    /* 1) Tap reaction overrides idle for its window, then falls back to idle. */
    if (s_reacting) {
        if (now < s_reactUntil) {
            if (now < s_nextFrameAt) return false;
            display_fb_clear(s_bg);
            int d = 0;
            if (!s_gif.playFrame(false, &d)) { s_gif.reset(); s_gif.playFrame(false, &d); } /* loop */
            s_nextFrameAt = now + (d > 0 ? (uint32_t)d : 100);
            return true;
        }
        /* reaction window over -> resume idle rotation from current rot. */
        s_reacting = false;
        s_animPause = 0;
        display_fb_clear(s_bg);
        if (openIdle(s_idleRot)) {
            int d = 0; s_gif.playFrame(false, &d);
            s_nextFrameAt = now + (d > 0 ? (uint32_t)d : 100);
            return true;
        }
        return false;
    }

    /* 2) IDLE: rotate idle_0..N with a dwell between GIFs. */
    if (!s_gifOpen) {
        if (s_animPause && now >= s_animPause) {
            s_animPause = 0;
            s_idleRot = (uint8_t)((s_idleRot + 1) % s_idleCount);
            display_fb_clear(s_bg);
            if (openIdle(s_idleRot)) {
                int d = 0; s_gif.playFrame(false, &d);
                s_nextFrameAt = now + (d > 0 ? (uint32_t)d : 100);
                return true;
            }
        }
        return false;
    }
    if (now < s_nextFrameAt) return false;

    display_fb_clear(s_bg);
    int d = 0;
    if (!s_gif.playFrame(false, &d)) {
        /* end of this idle GIF */
        s_gif.close(); s_gifOpen = false;
        if (s_idleCount == 1) {
            /* single idle: reopen immediately and keep looping */
            s_animPause = 0; s_idleRot = 0;
            if (openIdle(s_idleRot)) {
                int d2 = 0; s_gif.playFrame(false, &d2);
                s_nextFrameAt = now + (d2 > 0 ? (uint32_t)d2 : 100);
            }
        } else {
            s_animPause = now + ANIM_PAUSE_MS;   /* dwell, then rotate */
        }
        return true;
    }
    s_nextFrameAt = now + (d > 0 ? (uint32_t)d : 100);
    return true;
}

/* ── Public: tick ───────────────────────────────────────────────────────────
 * STEP 7 change: gif_tick() now COMPOSES the frog into s_fb but no longer flushes.
 * The HUD overlays its bottom strip AFTER this and the main loop flushes once, so
 * a single push shows frog+HUD together:
 *     bool drew = gif_tick();
 *     if (drew || hud_dirty()) { hud_draw(); display_flush(); }
 * Returns true iff a new frame was composited this call (s_fb changed). When the
 * frog frame isn't due, s_fb keeps the previous frog+HUD pixels intact, so a
 * HUD-only repaint (hud_dirty) flushes correct content without re-decoding.
 */
/* ── Always-on signature: "Cat Lab Brewing", centered below the frog ─────────
 * Overlay (transparent bg) so only the glyph pixels land — no box. Centered via
 * gfx_text_width so it stays centered if SIG_TEXT changes. Cheap (one short
 * string, clipped), so re-drawing it every composed frame is fine. */
static void draw_signature(void)
{
    if (!s_fb) return;
    int w = gfx_text_width(SIG_TEXT, SIG_SCALE);
    int x = (PET_W - w) / 2;
    if (x < 0) x = 0;
    gfx_text(s_fb, PET_W, PET_H, x, SIG_Y, SIG_TEXT, SIG_SCALE, SIG_COLOR, GFX_TRANSPARENT);
}

bool gif_tick(void)
{
    bool drew = tick_frame();            /* compose only; caller flushes */
    if (drew) draw_signature();          /* re-stamp the signature over the fresh frame */
    return drew;
}
