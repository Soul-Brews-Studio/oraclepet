/*
 * petclock.h — jc3248-pet-idf STEP 10: top-strip clock (HH:MM:SS + date).
 *
 * The pet is BLE-only (no Wi-Fi), so it cannot reach NTP itself. The host —
 * which IS NTP-synced — sends the current Unix epoch + local tz offset over the
 * BLE NUS heartbeat (ble.c parses "epoch"/"tzoff" into BleSnapshot). This module
 * stores that base and TICKS LOCALLY off esp_timer, so the clock advances
 * second-by-second between syncs and never freezes. It renders HH:MM:SS (left) and
 * the date (right) in the gap ABOVE the frog (the frog is centered at y>=50, so
 * [0,CLK_H) is free), drawn with the same byte-swapping gfx_text path the HUD
 * uses. Shows "--:--" until the first sync arrives.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Reserved top strip [0, CLK_H) — above the frog (which starts at y=50). */
#include "board.h"
#if BOARD_CLK_COMPACT
#define CLK_H  BOARD_TOP_H      /* one line: date left, time right */
#else
#define CLK_H  40
#endif

/* Cache display.c's framebuffer + the GIF background (call ONCE after gif_setup,
 * like hud_init). `bg` should match gif_bg() so the strip blends with the frog's
 * canvas. Returns false if the framebuffer is NULL (display_init not run). */
bool petclock_init(uint16_t bg);

/* Sync from a host heartbeat. epoch_utc = Unix UTC seconds; tzoff = local offset
 * in seconds (GMT+7 -> 25200). Ignored if epoch_utc < 0. Marks the clock synced. */
void petclock_set(long epoch_utc, long tzoff);

/* True once at least one sync has arrived. */
bool petclock_synced(void);

/* Recompute the current HH:MM:SS and return true iff it changed since the last
 * petclock_draw() — lets the main loop force a flush on a minute rollover even
 * when the frog is dwelling (drawing no new frames). */
bool petclock_tick_changed(void);

/* Clear the top strip to bg and draw time (left, white) + date (right, dim).
 * No-op if not inited. Call inside the same flush as the frog + HUD. */
void petclock_draw(void);

#ifdef __cplusplus
}
#endif
