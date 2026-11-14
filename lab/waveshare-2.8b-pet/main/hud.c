/*
 * hud.c — jc3248-pet-idf STEP 7: bottom HUD strip (pet name + status + conn dot).
 *
 * The IDF body has NO LovyanGFX/Adafruit_GFX, so the HUD is drawn by writing
 * host-order RGB565 directly into display.c's PSRAM framebuffer
 * (display_framebuffer()). All the text + shape blitting lives in hud_gfx.c — a
 * compact, dependency-free RGB565 primitive library (embedded 8x8 ASCII font,
 * gfx_text / gfx_fill_rect / gfx_hline / gfx_fill_circle). This file is just the
 * pet-specific LAYOUT + state, mirroring lab/jc3248-pet/src/pet.cpp::drawHUD().
 *
 * ── Byte order (verified against display.c + gif.cpp) ───────────────────────
 * display.c writes plain HOST-order RGB565 into s_fb (test pattern: red == 0xF800)
 * and it renders correctly; gif.cpp's display_fb_clear(s_bg) fills host-order RGB565
 * too. So the framebuffer is plain host RGB565 and hud_gfx writes the SAME way —
 * a 0xF800 literal is red here. (gif.cpp's BIG_ENDIAN_PIXELS only affects the bytes
 * AnimatedGIF packs into ITS palette; the on-fb representation is still host RGB565.)
 *
 * ── Layout (1:1 in spirit with pet.cpp::drawHUD; strip = [HUD_Y0=400, 480)) ──
 *   - full-strip clear to the GIF bg, then a 1px dim divider on the top edge,
 *   - connection dot (r=6) at (W-14, y0+18): green linked, gray otherwise,
 *   - line 1: pet name at (12, y0+8), size 3 (24px cell),
 *   - line 2: status/activity at (12, y0+44), size 2 (16px cell).
 * (The Arduino HUD also drew passkey-pair + 5h/wk usage lines; those need the
 *  BLE/usage data that arrives in step 9. hud_set_status() carries the one status
 *  line for now; name + dot are the always-present elements.)
 *
 * ── Render pipeline (STEP 7) ────────────────────────────────────────────────
 * hud_draw() composes into s_fb but does NOT flush; the main loop flushes once
 * after gif_tick()+hud_draw() so frog+HUD show in a single push. See the gif.cpp
 * + main.c edit spec returned with this deliverable.
 */
#include "hud.h"
#include "hud_gfx.h"      /* RGB565 font + primitives (the reusable blitter) */
#include "display.h"      /* DISP_W, DISP_H, display_framebuffer() */

#include <string.h>
#include <stdio.h>        /* snprintf for the usage windows */

/* ── Palette (host-order RGB565, same convention as display.c) ──────────────*/
#define HUD_BG_FALLBACK   0x0000        /* black, until hud_init() supplies the GIF bg */
#define HUD_TEXT          GFX_WHITE     /* white  — pal.text  */
#define HUD_TEXT_DIM      0xC618        /* light gray — GFX_GREY 0x8410 was too dim to read on-panel */
#define HUD_DOT_LINKED    GFX_GREEN     /* green  — BLE linked */
#define HUD_DOT_IDLE      GFX_GREY      /* gray   — placeholder / advertising */
#define HUD_PSI           0xE5A8        /* warm amber/yellow — the oracle Ψ mark */

/* ── State ──────────────────────────────────────────────────────────────────*/
static uint16_t *s_fb     = NULL;       /* display.c's framebuffer (host RGB565) */
static uint16_t  s_bg     = HUD_BG_FALLBACK;
static bool      s_dirty  = false;
static bool      s_link   = false;
static char      s_name[24]   = "pet";
static char      s_status[48] = "";
static int       s_pct5h = -1, s_pct7d = -1;       /* usage % (-1 = none) */
static long      s_reset5h = -1, s_reset7d = -1;   /* seconds-to-reset (-1 = none) */

/* ── Public API ──────────────────────────────────────────────────────────────*/

bool hud_init(uint16_t bg)
{
    s_fb = display_framebuffer();
    if (!s_fb) return false;
    s_bg    = bg;
    s_dirty = true;            /* first hud_draw() paints a full strip */
    return true;
}

bool hud_dirty(void) { return s_dirty; }

void hud_set_name(const char *name)
{
    const char *n = (name && name[0]) ? name : "pet";
    if (strncmp(s_name, n, sizeof(s_name)) == 0) return;
    strncpy(s_name, n, sizeof(s_name) - 1);
    s_name[sizeof(s_name) - 1] = 0;
    s_dirty = true;
}

void hud_set_status(const char *status)
{
    const char *st = status ? status : "";
    if (strncmp(s_status, st, sizeof(s_status)) == 0) return;
    strncpy(s_status, st, sizeof(s_status) - 1);
    s_status[sizeof(s_status) - 1] = 0;
    s_dirty = true;
}

void hud_set_link(bool linked)
{
    if (s_link == linked) return;
    s_link  = linked;
    s_dirty = true;
}

/* "5h 9% 2h54m" / "wk 26% 1d16h" — ported verbatim from pet.cpp fmt_reset/fmt_window. */
static void fmt_reset(char *out, size_t n, long sec) {
    if (sec < 0)           snprintf(out, n, "--");
    else if (sec >= 86400) snprintf(out, n, "%ldd%ldh", sec / 86400, (sec % 86400) / 3600);
    else if (sec >= 3600)  snprintf(out, n, "%ldh%02ldm", sec / 3600, (sec % 3600) / 60);
    else                   snprintf(out, n, "%ldm", sec / 60);
}
static void fmt_window(char *out, size_t n, const char *label, int pct, long reset) {
    char r[12]; fmt_reset(r, sizeof(r), reset);
    if (pct >= 0 && reset >= 0)  snprintf(out, n, "%s %d%% %s", label, pct, r);
    else if (pct >= 0)           snprintf(out, n, "%s %d%%", label, pct);
    else                         snprintf(out, n, "%s %s", label, r);
}

void hud_set_usage(int pct5h, long reset5h, int pct7d, long reset7d)
{
    if (s_pct5h == pct5h && s_reset5h == reset5h && s_pct7d == pct7d && s_reset7d == reset7d) return;
    s_pct5h = pct5h; s_reset5h = reset5h; s_pct7d = pct7d; s_reset7d = reset7d;
    s_dirty = true;
}

/* A big Greek capital Psi (Ψ) brand mark, built from rects (the 8x8 font has no
 * Greek glyphs): a left + right top prong, a full-height center stem, a bowl bar
 * joining the prong feet, and a base foot. gfx_fill_rect byte-swaps internally,
 * so the colour is passed host-order. (x0,y0) = bounding-box top-left, w x h. */
static void draw_psi(int x0, int y0, int w, int h, uint16_t color)
{
    int t  = w / 6; if (t < 3) t = 3;          /* stroke thickness     */
    int ph = (h * 9) / 20;                      /* top prong height ~45% */
    int cx = x0 + (w - t) / 2;                  /* center stem x         */
    gfx_fill_rect(s_fb, DISP_W, DISP_H, x0,         y0,          t, ph, color);  /* left prong  */
    gfx_fill_rect(s_fb, DISP_W, DISP_H, x0 + w - t, y0,          t, ph, color);  /* right prong */
    gfx_fill_rect(s_fb, DISP_W, DISP_H, cx,         y0,          t, h,  color);  /* center stem */
    gfx_fill_rect(s_fb, DISP_W, DISP_H, x0,         y0 + ph - t, w, t,  color);  /* bowl bar    */
    int fw = w / 2;
    gfx_fill_rect(s_fb, DISP_W, DISP_H, x0 + (w - fw) / 2, y0 + h - t, fw, t, color);  /* foot */
}

void hud_draw(void)
{
    if (!s_fb) return;
    const int y0 = HUD_Y0;             /* 400 */

    /* 1) Clear the strip to the GIF background, then a 1px dim divider on top. */
    gfx_fill_rect(s_fb, DISP_W, DISP_H, 0, y0, HUD_W, HUD_H, s_bg);
    gfx_hline(s_fb, DISP_W, DISP_H, 0, y0, HUD_W, HUD_TEXT_DIM);

    /* 2) Bottom-right: the big amber Psi (Ψ) with the connection dot centered
     *    just above it (green linked, gray idle). */
    {
        const int pw = 36, ph = 52;
        const int px = HUD_W - pw - 8;
        const int py = y0 + HUD_H - ph - 6;
        gfx_fill_circle(s_fb, DISP_W, DISP_H, px + pw / 2, py - 8, 5,
                        s_link ? HUD_DOT_LINKED : HUD_DOT_IDLE);
        draw_psi(px, py, pw, ph, HUD_PSI);
    }

    /* 3) Line 1 — pet name, size 3 (white). Matches pet.cpp drawString(name, 12, y0+6). */
    gfx_text(s_fb, DISP_W, DISP_H, 12, y0 + 6, s_name, 3, HUD_TEXT, GFX_TRANSPARENT);

    /* 4) Lines 2-3 — the two usage windows "5h 9% 2h54m" / "wk 26% 1d16h" when the
     *    heartbeat carried usage (1:1 with pet.cpp::drawHUD: l5 white, wk dim). If no
     *    usage yet, fall back to a single status/activity line. */
    bool have_usage = (s_pct5h >= 0 || s_pct7d >= 0 || s_reset5h >= 0 || s_reset7d >= 0);
    if (have_usage) {
        char l5[40], l7[40];
        fmt_window(l5, sizeof(l5), "5h", s_pct5h, s_reset5h);
        fmt_window(l7, sizeof(l7), "wk", s_pct7d, s_reset7d);
        gfx_text(s_fb, DISP_W, DISP_H, 12, y0 + 34, l5, 2, HUD_TEXT,     GFX_TRANSPARENT);
        gfx_text(s_fb, DISP_W, DISP_H, 12, y0 + 56, l7, 2, HUD_TEXT_DIM, GFX_TRANSPARENT);
    } else if (s_status[0]) {
        /* idle: a single status line — left-aligned, a little space above it. */
        gfx_text(s_fb, DISP_W, DISP_H, 12, y0 + 40, s_status, 2, HUD_TEXT_DIM, GFX_TRANSPARENT);
    }

    s_dirty = false;
}
