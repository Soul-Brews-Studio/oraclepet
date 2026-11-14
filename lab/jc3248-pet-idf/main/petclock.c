/*
 * petclock.c — jc3248-pet-idf STEP 10: top-strip clock.
 *
 * Stores the host's epoch+tzoff (the local "wall epoch" = epoch_utc + tzoff) plus
 * the esp_timer millis() at sync; current local time = base_local + elapsed. We
 * decode it with gmtime_r() on the PRE-OFFSET seconds (so gmtime's UTC breakdown
 * IS the local wall clock — no tzset/localtime needed) and strftime the strings.
 * Drawn via hud_gfx (gfx_text byte-swaps for the big-endian panel, like the HUD).
 */
#include "petclock.h"
#include "hud_gfx.h"     /* gfx_text / gfx_fill_rect / gfx_text_width */
#include "display.h"     /* DISP_W, DISP_H, display_framebuffer() */
#include "esp_timer.h"
#include "esp_log.h"

#include <time.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "clock";

/* ── Palette + layout (host-order RGB565; gfx_text byte-swaps) ───────────────*/
#define CLK_TIME_COLOR  GFX_WHITE     /* HH:MM:SS — bright, primary            */
#define CLK_DATE_COLOR  0xC618        /* date  — dim gray (= HUD_TEXT_DIM)     */
#define CLK_SCALE       2             /* 16px-tall glyphs                       */
#define CLK_Y           12            /* baseline-top inside [0,CLK_H)          */
#define CLK_MARGIN      12

/* ── State ──────────────────────────────────────────────────────────────────*/
static uint16_t *s_fb         = NULL;
static uint16_t  s_bg         = 0x0000;
static bool      s_synced     = false;
static long      s_base_local = 0;            /* epoch_utc + tzoff at last sync */
static uint32_t  s_base_ms    = 0;            /* millis() at last sync          */
static char      s_last_hhmm[9] = "";         /* last drawn HH:MM:SS (change gate) */

static inline uint32_t millis(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

bool petclock_init(uint16_t bg)
{
    s_fb = display_framebuffer();
    if (!s_fb) return false;
    s_bg = bg;
    return true;
}

void petclock_set(long epoch_utc, long tzoff)
{
    if (epoch_utc < 0) return;
    /* The clock runs on esp_timer between syncs; BLE only corrects it. Hosts send whole
     * seconds (truncated) every few seconds, so a reading within 1 s of ours is jitter,
     * not drift: re-basing on it would make the seconds stutter. Re-base only on real
     * drift, a timezone change, or the first sync. */
    if (s_synced) {
        long ours = s_base_local + (long)((millis() - s_base_ms) / 1000u);
        long diff = (epoch_utc + tzoff) - ours;
        if (diff >= -1 && diff <= 1) return;
        ESP_LOGI(TAG, "time re-synced: drift %ld s", diff);
    }
    s_base_local = epoch_utc + tzoff;
    s_base_ms    = millis();
    bool first   = !s_synced;
    s_synced     = true;
    if (first) ESP_LOGI(TAG, "time synced: local epoch %ld (utc %ld, tzoff %ld)",
                        s_base_local, epoch_utc, tzoff);
}

bool petclock_synced(void) { return s_synced; }

/* Current local wall time -> broken-down tm (gmtime on the pre-offset seconds). */
static bool now_tm(struct tm *out)
{
    if (!s_synced) return false;
    time_t t = (time_t)(s_base_local + (long)((millis() - s_base_ms) / 1000u));
    gmtime_r(&t, out);
    return true;
}

bool petclock_tick_changed(void)
{
    char hhmm[9];
    struct tm tm;
    if (now_tm(&tm)) strftime(hhmm, sizeof(hhmm), "%H:%M:%S", &tm);
    else             snprintf(hhmm, sizeof(hhmm), "--:--:--");
    return strcmp(hhmm, s_last_hhmm) != 0;
}

void petclock_draw(void)
{
    if (!s_fb) return;

    char hhmm[9], date[16];
    struct tm tm;
    if (now_tm(&tm)) {
        strftime(hhmm, sizeof(hhmm), "%H:%M:%S", &tm);   /* 8 chars x 16 px = 128 px */
        strftime(date, sizeof(date), "%a %d %b", &tm);   /* e.g. "Fri 30 May" */
    } else {
        snprintf(hhmm, sizeof(hhmm), "--:--:--");
        date[0] = 0;
    }

    /* Clear the strip (above the frog at y>=50), then a TOP-RIGHT clock: HH:MM:SS
     * over the date, both right-aligned to the right margin. */
    gfx_fill_rect(s_fb, DISP_W, DISP_H, 0, 0, DISP_W, CLK_H, s_bg);
#if BOARD_CLK_COMPACT
    /* small screen: one line, HH:MM:SS right (16 px), the date left (8 px) */
    {
        int tw2 = gfx_text_width(hhmm, CLK_SCALE);
        gfx_text(s_fb, DISP_W, DISP_H, DISP_W - tw2 - 6, 2, hhmm, CLK_SCALE, CLK_TIME_COLOR, GFX_TRANSPARENT);
        if (date[0]) gfx_text(s_fb, DISP_W, DISP_H, 6, 6, date, 1, CLK_DATE_COLOR, GFX_TRANSPARENT);
        strncpy(s_last_hhmm, hhmm, sizeof(s_last_hhmm) - 1);
        s_last_hhmm[sizeof(s_last_hhmm) - 1] = 0;
        return;
    }
#endif
    int tw = gfx_text_width(hhmm, CLK_SCALE);
    gfx_text(s_fb, DISP_W, DISP_H, DISP_W - tw - CLK_MARGIN, 2, hhmm, CLK_SCALE,
             CLK_TIME_COLOR, GFX_TRANSPARENT);
    if (date[0]) {
        int dw = gfx_text_width(date, CLK_SCALE);
        int x = DISP_W - dw - CLK_MARGIN;
        if (x < 0) x = 0;
        gfx_text(s_fb, DISP_W, DISP_H, x, 2 + GFX_FONT_H * CLK_SCALE + 2, date,
                 CLK_SCALE, CLK_DATE_COLOR, GFX_TRANSPARENT);
    }

    strncpy(s_last_hhmm, hhmm, sizeof(s_last_hhmm) - 1);
    s_last_hhmm[sizeof(s_last_hhmm) - 1] = 0;
}
