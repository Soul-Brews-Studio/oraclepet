/*
 * display.h — waveshare-2.8b-pet
 *
 * Board: Waveshare ESP32-S3-LCD-2.8B
 *   Display : 480×640 RGB565, ST7701S controller
 *   Init    : SPI (CLK=GPIO2, MOSI=GPIO1, CS via TCA9554 EXIO2)
 *   RST     : TCA9554 EXIO0
 *   RGB I/F : DE=40 HSYNC=38 VSYNC=39 PCLK=41, 16 MHz, active high
 *   Backlight: GPIO6 LEDC PWM
 *   No touch on this variant.
 *
 * The API is identical to jc3248-pet-idf/display.h so all upstream callers
 * (gif.cpp, hud.c, petclock.c) compile unchanged.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Panel geometry (portrait native) ----------------------------------- */
#define DISP_W 480
#define DISP_H 640

/* ---- I2C (TCA9554 expander + future peripherals) ------------------------ */
#define DISP_I2C_SDA   15
#define DISP_I2C_SCL    7

/* ---- TCA9554 at 0x20 — EXIO bit assignments ----------------------------- */
#define TCA9554_ADDR    0x20
#define TCA9554_EXIO_RST  0   /* LCD hardware reset (active low) */
#define TCA9554_EXIO_CS   2   /* ST7701S SPI CS (active low)    */
#define TCA9554_EXIO_BUZ  7   /* passive buzzer (high=on)        */

/* ---- SPI (ST7701S init channel only — one-shot at boot) ----------------- */
#define DISP_SPI_CLK    2
#define DISP_SPI_MOSI   1

/* ---- RGB parallel pins -------------------------------------------------- */
#define DISP_PIN_DE    40
#define DISP_PIN_HSYNC 38
#define DISP_PIN_VSYNC 39
#define DISP_PIN_PCLK  41
/* B0..B4 */
#define DISP_PINS_B  { 5, 45, 48, 47, 21 }
/* G0..G5 */
#define DISP_PINS_G  { 14, 13, 12, 11, 10, 9 }
/* R0..R4 */
#define DISP_PINS_R  { 46,  3,  8, 18, 17 }

/* ---- Backlight ---------------------------------------------------------- */
#define DISP_PIN_BL   6

/*
 * Initialize everything:
 *   1. I2C master (TCA9554)
 *   2. TCA9554 all-outputs; RST-cycle the ST7701S
 *   3. SPI ST7701S init sequence
 *   4. esp_lcd_rgb_panel; allocate PSRAM framebuffer
 *   5. LEDC backlight on
 */
esp_err_t display_init(void);

/* Raw esp_lcd panel handle (NULL until display_init succeeds). */
esp_lcd_panel_handle_t display_panel(void);

/* Set backlight 0..255. */
void display_set_backlight(uint8_t level);

/*
 * Flush: copy the PSRAM framebuffer to the RGB panel.
 * For RGB panels the DMA streams continuously; flush() just synchronises
 * by swapping/invalidating if double-buffered, or is a no-op (single buf).
 * Callers always call this — it's safe to make it a no-op here.
 */
esp_err_t display_flush(void);

/* Fill framebuffer with one RGB565 color (host byte order) and flush. */
esp_err_t display_fill_solid(uint16_t rgb565);

/* Draw a test pattern (color bars) and flush. */
esp_err_t display_test_pattern(void);

/* Direct pointer to the RGB565 framebuffer (DISP_W×DISP_H words), or NULL. */
uint16_t *display_framebuffer(void);

/* Fill framebuffer with one RGB565 color WITHOUT flushing. */
void display_fb_clear(uint16_t rgb565);

#ifdef __cplusplus
}
#endif
