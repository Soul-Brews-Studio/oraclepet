/*
 * display.h — jc3248-pet-idf STEP 2: AXS15231B QSPI panel bring-up.
 *
 * Board: Guition JC3248W535 (ESP32-S3). AXS15231B 320x480 QSPI AMOLED-style panel.
 * Uses the managed component `espressif/esp_lcd_axs15231b` ==2.1.0 over esp_lcd.
 *
 * Step-2 scope ONLY: bus + panel-io + panel create, our JC3248W535 vendor init
 * table, LEDC backlight, cold-boot non-black warm-up, and solid/test-pattern
 * fills from a PSRAM framebuffer. NO GIF / touch / audio / BLE yet.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"   /* esp_lcd_panel_handle_t, draw_bitmap, disp_on_off */
#include "board.h"               /* DISP_W / DISP_H and the layout for this board */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Panel geometry (portrait native): DISP_W x DISP_H from board.h ---- */

#if defined(PET_BOARD_LCD28)
/* ---- Pins (2.8" ESP32-S3 board; same as xiaozhi's freenove-esp32s3-display-2.8-lcd) ----
 * SPI3: SCLK=12 MOSI=11 MISO=13 CS=10 DC=46 (no RST pin)   Backlight: GPIO45, active high */
#define DISP_PIN_SCLK 12
#define DISP_PIN_MOSI 11
#define DISP_PIN_MISO 13
#define DISP_PIN_CS   10
#define DISP_PIN_DC   46
#define DISP_PIN_BL   45
#elif defined(PET_BOARD_SC01)
/* ---- Pins (WT32-SC01 Plus; lab/buddy-port/src/lgfx_sc01plus.h, lab/wt32-sc01-plus) ----
 * i80 8-bit: WR=47 DC=0 RST=4, D0..D7 = 9 46 3 8 18 17 16 15, CS tied low, no RD.
 * Backlight: GPIO45, active high. */
#define DISP_PIN_WR   47
#define DISP_PIN_DC    0
#define DISP_PIN_RST   4
#define DISP_PIN_D0    9
#define DISP_PIN_D1   46
#define DISP_PIN_D2    3
#define DISP_PIN_D3    8
#define DISP_PIN_D4   18
#define DISP_PIN_D5   17
#define DISP_PIN_D6   16
#define DISP_PIN_D7   15
#define DISP_PIN_BL   45
#else
/* ---- Pins (verified on the JC3248W535) ----
 * QSPI:  SCLK=47  D0=21  D1=48  D2=40  D3=39  CS=45   (no DC, no hw RST pin)
 * Backlight: GPIO1 via LEDC (25kHz/8-bit — NOT the vendor 5kHz, which sings).
 */
#define DISP_PIN_SCLK 47
#define DISP_PIN_D0   21
#define DISP_PIN_D1   48
#define DISP_PIN_D2   40
#define DISP_PIN_D3   39
#define DISP_PIN_CS   45
#define DISP_PIN_BL    1
#endif

/*
 * Initialize the display: SPI2 QSPI bus -> esp_lcd panel-io -> AXS15231B panel
 * (with OUR JC3248W535 vendor init table) -> reset/init/disp-on, then LEDC
 * backlight on, then a cold-boot non-black warm-up (only on POWERON).
 *
 * Allocates a full-screen RGB565 framebuffer in PSRAM (kept internally; reused
 * by display_fill_solid / display_test_pattern / display_flush).
 *
 * Returns ESP_OK on success; on failure the panel handle is left NULL and the
 * caller should log + continue (step 2 is a diagnostic, not fatal to boot).
 */
esp_err_t display_init(void);

/* Raw esp_lcd panel handle (NULL until display_init succeeds). */
esp_lcd_panel_handle_t display_panel(void);

/* Set backlight 0..255 (LEDC duty). 0 = off, 255 = full. */
void display_set_backlight(uint8_t level);

/*
 * Push the entire internal PSRAM framebuffer to the panel (full-screen, y_start=0
 * so the QSPI path issues a fresh RAMWR — see display.c for why that matters).
 */
esp_err_t display_flush(void);

/* Push only rows [y, y + rows) — the fast path for a small animated strip (the voice waveform):
 * on the lcd28 that is a few KB of SPI DMA instead of the whole 150 KB frame. The JC3248's
 * QSPI panel can only take whole frames (no row address), so there it flushes everything. */
esp_err_t display_flush_rows(int y, int rows);

/* Fill the internal framebuffer with one RGB565 color (host byte order) and flush. */
esp_err_t display_fill_solid(uint16_t rgb565);

/* Draw a diagnostic test pattern (vertical color bars + corner markers) and flush. */
esp_err_t display_test_pattern(void);

/* Pointer to the internal RGB565 framebuffer (DISP_W*DISP_H), or NULL if not inited. */
uint16_t *display_framebuffer(void);

/*
 * Fill the internal framebuffer with one RGB565 color WITHOUT pushing to the
 * panel. This is the cheap per-frame background clear used by the GIF renderer
 * (the Arduino pet did spr.fillSprite(bg) before composing each frame); the
 * caller flushes once, after composing, via display_flush(). No-op if not inited.
 */
void display_fb_clear(uint16_t rgb565);

#ifdef __cplusplus
}
#endif
