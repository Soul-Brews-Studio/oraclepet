/*
 * hud.h — jc3248-pet-idf STEP 7: bottom HUD strip (hand-rolled RGB565 blitter).
 *
 * The IDF body has NO LovyanGFX/Adafruit_GFX, so the HUD is drawn by writing
 * RGB565 pixels directly into display.c's PSRAM framebuffer (display_framebuffer()).
 * This mirrors the proven Arduino layout in lab/jc3248-pet/src/pet.cpp::drawHUD():
 * the frog is centered in the top region (y in [0, PET_H - HUD_H)); the HUD owns
 * the bottom strip (HUD_Y0 .. PET_H). The GIF placement (gif.cpp::gifPlace) never
 * draws into that strip, so the frog and HUD never overlap.
 *
 *   ┌──────────────────────────────┐  y=0
 *   │                              │
 *   │         3x frog (GIF)        │
 *   │                              │
 *   ├──────────────────────────● ──┤  y=400  HUD_Y0  (divider + link dot)
 *   │ bufo            (name, sz3)  │
 *   │ idle  BLE adv  0:42 (status) │  y=480  PET_H
 *   └──────────────────────────────┘
 *
 * ── Render-pipeline contract (STEP 7 integration) ──────────────────────────
 * The HUD overlays the frog AFTER the GIF composes its frame and BEFORE the
 * single per-frame display_flush(), so one flush shows frog+HUD together. The
 * cleanest, lowest-risk refactor (see hud.c header comment for the exact gif.cpp
 * + main.c edits):
 *   - gif_tick() composes into s_fb but NO LONGER flushes (it returns true iff it
 *     drew a new frame this call).
 *   - the main loop does:
 *         bool drew = gif_tick();
 *         if (drew || hud_dirty()) { hud_draw(); display_flush(); }
 * The HUD writes only into its reserved bottom strip, which the GIF never paints,
 * so even on a frog-only frame the HUD pixels from the previous flush survive in
 * s_fb and are pushed again intact.
 */
#pragma once
#include "board.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── HUD geometry (matches pet.cpp: PET_W=320, PET_H=480, PET_HUD_H=80) ──────
 * The frog GIF is centered in [0, HUD_Y0); the HUD owns [HUD_Y0, PET_H).
 */
#define HUD_W      DISP_W
#define HUD_H      BOARD_HUD_H
#define HUD_Y0     (DISP_H - HUD_H)     /* 400 on the JC3248, 244 on lcd28 */

/*
 * Initialize the HUD against display.c's framebuffer (cache display_framebuffer()
 * and the background color used to clear the strip each repaint). Call ONCE after
 * display_init() and gif_setup() (so the framebuffer exists and the bg color is
 * known). `bg` should be the SAME RGB565 background the GIF renderer clears to
 * (gif.cpp's s_bg) so the strip blends with the frog's canvas. Marks the HUD dirty
 * so the first hud_draw() paints a full strip.
 *
 * Returns false if the framebuffer is NULL (display_init() not run).
 */
bool hud_init(uint16_t bg);

/*
 * Compose the HUD into the bottom strip of the framebuffer (does NOT flush — the
 * caller flushes once after gif_tick()+hud_draw() so frog+HUD show in one push).
 * Clears the HUD_DIRTY flag. Safe to call every frame; it only repaints the
 * reserved bottom strip, never the frog region. No-op if hud_init() didn't run.
 */
void hud_draw(void);

/*
 * True iff HUD content changed since the last hud_draw() (status/link/name set).
 * The main loop uses this to force a flush on a frog-idle frame:
 *     if (gif_tick() || hud_dirty()) { hud_draw(); display_flush(); }
 */
bool hud_dirty(void);

/* ── Setters (mark the HUD dirty) — wired by later steps (BLE = step 9) ──────*/

/*
 * Pet name shown big (size-3 font) on HUD line 1. Typically the character-pack
 * name ("bufo") or the oracle name ("esp32-oracle"). Copied internally; pass NULL
 * or "" to fall back to "pet". Mirrors pet.cpp's petName.
 */
void hud_set_name(const char *name);

/*
 * Status / activity line shown smaller (size-2) on HUD line 2. e.g. "idle",
 * "BLE adv", "run 1  wait 0". Copied internally (truncated to fit). Until BLE
 * lands (step 9) the app sets a static placeholder like "idle  BLE off".
 * Mirrors pet.cpp's status line.
 */
void hud_set_status(const char *status);

/*
 * Connection link state for the dot in the top-right of the strip:
 *   false -> gray  (placeholder / not linked / advertising)
 *   true  -> green (BLE linked)
 * (The Arduino HUD also had an orange "pairing" color keyed off a passkey; that
 *  is deferred to step 9 — hud_set_link is the boolean placeholder for now.)
 */
void hud_set_link(bool linked);

/* Usage windows (matches pet.cpp): pct -1 = none, reset = seconds-to-reset (-1 = none).
 * When any field >= 0, hud_draw shows two lines: "5h NN% Xh Ym" / "wk NN% Xd Yh". */
void hud_set_usage(int pct5h, long reset5h, int pct7d, long reset7d);

/* DBK link (dbk.h): the air-quality line, e.g. "PM2.5 12 good E", in `color`, and a small second
 * line "2 pets  kit ok". line "" hides it. With usage present both share one scale-1 line under
 * the usage windows; without usage the reading is drawn scale 2 where the status line was. */
void hud_set_air(const char *line, const char *line2, uint16_t color);

#ifdef __cplusplus
}
#endif
