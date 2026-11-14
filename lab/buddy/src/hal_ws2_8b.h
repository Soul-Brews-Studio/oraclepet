#pragma once
/*
 * hal_ws2_8b.h — Waveshare ESP32-S3-LCD-2.8B (480×640 RGB, ST7701S, TCA9554)
 *
 * Uses ESP-IDF esp_lcd_rgb_panel directly (same API as ESPHome st7701s under
 * the hood) — matches the proven lab/waveshare-2.8b/waveshare-2.8b.yaml config.
 *
 * NO Serial in the bring-up path: this board's USB-Serial-JTAG is unreadable on
 * the host, and HWCDC Serial.printf can BLOCK with no monitor attached. All
 * diagnostics go over the BUZZER (TCA9554 EXIO7).
 *
 * BEEP MAP (count carefully):
 *   1 short  = TCA9554 alive
 *   2 short  = ST7701S SPI init done
 *   3 short  = reached RGB-panel create
 *   4 short  = esp_lcd_new_rgb_panel OK
 *   5 short  = esp_lcd_panel_init OK
 *   6 short  = framebuffer OK  -> then RED/GREEN/BLUE colour sweep
 *   LONG + N = a call FAILED, N = (err & 0x0F):
 *              1 NO_MEM  2 INVALID_ARG  3 INVALID_STATE  4 INVALID_SIZE
 *              5 NOT_FOUND  6 NOT_SUPPORTED  (~15 = ESP_FAIL)
 *   LONG + 9 = framebuffer pointer was NULL
 */

#if defined(BOARD_WAVESHARE_2_8B)

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <Wire.h>
#include <SPI.h>
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_heap_caps.h"

// ── Pin map ──────────────────────────────────────────────────────────────────
#define WS2_I2C_SDA   15
#define WS2_I2C_SCL    7
#define WS2_SPI_CLK    2
#define WS2_SPI_MOSI   1
#define WS2_PIN_BL     6

#define WS2_TCA_ADDR   0x20
#define WS2_TCA_RST    0   // EXIO0 = display RST (active low)
#define WS2_TCA_CS     2   // EXIO2 = ST7701S SPI CS (active low)
#define WS2_TCA_BUZ    7   // EXIO7 = passive buzzer (active high)

// ── LGFX device (sprite parent only — RGB panel driven by ESP-IDF) ───────────
class LGFX_WS2_8B : public lgfx::LGFX_Device {
public:
  LGFX_WS2_8B() {}
};

extern LGFX_WS2_8B tft;

// ── ws2_8b namespace ─────────────────────────────────────────────────────────
namespace ws2_8b {

static esp_lcd_panel_handle_t s_panel = nullptr;
static uint16_t              *s_fb    = nullptr;

// ── TCA9554 helpers ───────────────────────────────────────────────────────────
static uint8_t s_tca_out = 0;

static void tca_write_reg(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(WS2_TCA_ADDR);
    Wire.write(reg);
    Wire.write(val);
    Wire.endTransmission();
}

static void tca_set_bit(uint8_t bit, bool high) {
    if (high) s_tca_out |= (uint8_t)(1u << bit);
    else      s_tca_out &= (uint8_t)~(1u << bit);
    tca_write_reg(0x01, s_tca_out);
}

// ── Buzzer diagnostics ────────────────────────────────────────────────────────
static void beep(int n) {
    for (int i = 0; i < n; i++) {
        tca_set_bit(WS2_TCA_BUZ, true);  delay(80);
        tca_set_bit(WS2_TCA_BUZ, false); delay(160);
    }
}

static void long_beep() {
    tca_set_bit(WS2_TCA_BUZ, true);  delay(650);
    tca_set_bit(WS2_TCA_BUZ, false); delay(400);
}

// Loop forever, UNAMBIGUOUS:  STEP as LONG beeps, then ERROR as short beeps.
//   step: 1 LONG = new_rgb_panel failed   2 LONG = panel_init failed   3 LONG = null framebuffer
//   error nibble (short): 1 NO_MEM  2 INVALID_ARG  3 INVALID_STATE  4 INVALID_SIZE
//                         5 NOT_FOUND  6 NOT_SUPPORTED  (10 = ESP_OK/other)
static void fail_report(int step, esp_err_t err) {
    int n = (int)(err & 0x0F);
    if (n == 0) n = 10;
    while (true) {
        delay(2000);
        for (int i = 0; i < step; i++) long_beep();  // which call failed
        delay(1500);
        beep(n);                                       // error code
        delay(2000);
    }
}

// ── ST7701S vendor init sequence (from waveshare-2.8b.yaml, proven on HW) ────
// Format: {len, cmd, d0..dn}  len=0 → delay(cmd ms)  len=0,cmd=0 → end
// clang-format off
static const uint8_t s_st7701s_seq[] = {
  6, 0xFF, 0x77, 0x01, 0x00, 0x00, 0x13,
  2, 0xEF, 0x08,
  6, 0xFF, 0x77, 0x01, 0x00, 0x00, 0x10,
  3, 0xC0, 0x4F, 0x00,
  3, 0xC1, 0x10, 0x02,
  3, 0xC2, 0x07, 0x02,
  2, 0xCC, 0x10,
 17, 0xB0, 0x00, 0x10, 0x17, 0x0D, 0x11, 0x06, 0x05, 0x08, 0x07, 0x1F, 0x04, 0x11, 0x0E, 0x29, 0x30, 0x1F,
 17, 0xB1, 0x00, 0x0D, 0x14, 0x0E, 0x11, 0x06, 0x04, 0x08, 0x08, 0x20, 0x05, 0x13, 0x13, 0x26, 0x30, 0x1F,
  6, 0xFF, 0x77, 0x01, 0x00, 0x00, 0x11,
  2, 0xB0, 0x65,
  2, 0xB1, 0x71,
  2, 0xB2, 0x82,
  2, 0xB3, 0x80,
  2, 0xB5, 0x42,
  2, 0xB7, 0x85,
  2, 0xB8, 0x20,
  2, 0xC0, 0x09,
  2, 0xC1, 0x78,
  2, 0xC2, 0x78,
  2, 0xD0, 0x88,
  2, 0xEE, 0x42,
  4, 0xE0, 0x00, 0x00, 0x02,
 12, 0xE1, 0x04, 0xA0, 0x06, 0xA0, 0x05, 0xA0, 0x07, 0xA0, 0x00, 0x44, 0x44,
 13, 0xE2, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  5, 0xE3, 0x00, 0x00, 0x22, 0x22,
  3, 0xE4, 0x44, 0x44,
 17, 0xE5, 0x0C, 0x90, 0xA0, 0xA0, 0x0E, 0x92, 0xA0, 0xA0, 0x08, 0x8C, 0xA0, 0xA0, 0x0A, 0x8E, 0xA0, 0xA0,
  5, 0xE6, 0x00, 0x00, 0x22, 0x22,
  3, 0xE7, 0x44, 0x44,
 17, 0xE8, 0x0D, 0x91, 0xA0, 0xA0, 0x0F, 0x93, 0xA0, 0xA0, 0x09, 0x8D, 0xA0, 0xA0, 0x0B, 0x8F, 0xA0, 0xA0,
  8, 0xEB, 0x00, 0x00, 0xE4, 0xE4, 0x44, 0x00, 0x40,
 17, 0xED, 0xFF, 0xF5, 0x47, 0x6F, 0x0B, 0xA1, 0xAB, 0xFF, 0xFF, 0xBA, 0x1A, 0xB0, 0xF6, 0x74, 0x5F, 0xFF,
  7, 0xEF, 0x08, 0x08, 0x08, 0x40, 0x3F, 0x64,
  6, 0xFF, 0x77, 0x01, 0x00, 0x00, 0x00,
  6, 0xFF, 0x77, 0x01, 0x00, 0x00, 0x13,
  3, 0xE6, 0x16, 0x7C,
  3, 0xE8, 0x00, 0x0E,
  6, 0xFF, 0x77, 0x01, 0x00, 0x00, 0x00,
  1, 0x11,       // SLPOUT
  0, 200,        // delay 200ms
  6, 0xFF, 0x77, 0x01, 0x00, 0x00, 0x13,
  3, 0xE8, 0x00, 0x0C,
  0, 150,        // delay 150ms
  3, 0xE8, 0x00, 0x00,
  6, 0xFF, 0x77, 0x01, 0x00, 0x00, 0x00,
  2, 0x35, 0x00, // TE on
  1, 0x11,       // SLPOUT (ESPHome sends this in C++)
  0, 120,        // delay 120ms
  1, 0x29,       // DISPLAY ON (ESPHome sends this in C++ — the YAML seq omits it)
  0, 0,          // end
};
// clang-format on

static void st7701s_send(uint8_t cmd, const uint8_t *data, uint8_t len) {
    tca_set_bit(WS2_TCA_CS, false);
    SPI.transfer(cmd);
    for (uint8_t i = 0; i < len; i++) SPI.transfer(data[i]);
    tca_set_bit(WS2_TCA_CS, true);
}

static void st7701s_init_sequence() {
    SPI.begin(WS2_SPI_CLK, -1, WS2_SPI_MOSI, -1);
    SPI.setFrequency(500000);
    SPI.setDataMode(SPI_MODE0);
    SPI.setBitOrder(MSBFIRST);
    const uint8_t *p = s_st7701s_seq;
    while (true) {
        uint8_t len = *p++;
        if (len == 0) {
            uint8_t ms = *p++;
            if (ms == 0) break;
            delay(ms);
        } else {
            uint8_t cmd = *p++;
            st7701s_send(cmd, p, len - 1);
            p += (len - 1);
        }
    }
    SPI.end();
}

// ── init() — call from HalM5::begin() ────────────────────────────────────────
static void init() {
    // 1. TCA9554: all outputs, buzzer off
    Wire.begin(WS2_I2C_SDA, WS2_I2C_SCL, 400000);
    delay(10);
    tca_write_reg(0x03, 0x00);                           // config: all outputs
    s_tca_out = (uint8_t)(0xFF & ~(1u << WS2_TCA_BUZ)); // CS=H, RST=H, buzzer=OFF
    tca_write_reg(0x01, s_tca_out);
    delay(10);
    beep(1);  // ✓ TCA9554 alive

    // 2. ST7701S hardware reset + vendor init via SPI
    tca_set_bit(WS2_TCA_RST, false); delay(10);
    tca_set_bit(WS2_TCA_RST, true);  delay(120);
    st7701s_init_sequence();
    beep(2);  // ✓ SPI init done

    // 3. Backlight on (Arduino-ESP32 3.x API)
    ledcAttach(WS2_PIN_BL, 5000, 8);
    ledcWrite(WS2_PIN_BL, 200);

    // 4. RGB panel via ESP-IDF
    esp_lcd_rgb_panel_config_t cfg = {};
    cfg.clk_src                   = LCD_CLK_SRC_PLL160M;  // DEFAULT can't reach 16MHz PCLK
    cfg.timings.pclk_hz           = 16 * 1000 * 1000;
    cfg.timings.h_res             = 480;
    cfg.timings.v_res             = 640;
    cfg.timings.hsync_pulse_width = 10;
    cfg.timings.hsync_back_porch  = 70;
    cfg.timings.hsync_front_porch = 60;
    cfg.timings.vsync_pulse_width = 10;
    cfg.timings.vsync_back_porch  = 20;
    cfg.timings.vsync_front_porch = 20;
    cfg.timings.flags.pclk_active_neg = 0;
    cfg.data_width                = 16;
    cfg.num_fbs                   = 1;
    cfg.bounce_buffer_size_px     = 480 * 10;  // ESPHome: width*10 (proven)
    // NOTE: do NOT set bits_per_pixel / psram_trans_align / sram_trans_align —
    // ESPHome leaves them 0 and it works; setting them made panel_init fail.
    cfg.hsync_gpio_num            = 38;
    cfg.vsync_gpio_num            = 39;
    cfg.de_gpio_num               = 40;
    cfg.pclk_gpio_num             = 41;
    cfg.disp_gpio_num             = -1;
    // B0-B4, G0-G5, R0-R4 (matches YAML blue/green/red pin order)
    const int gpios[16] = {5, 45, 48, 47, 21, 14, 13, 12, 11, 10, 9, 46, 3, 8, 18, 17};
    for (int i = 0; i < 16; i++) cfg.data_gpio_nums[i] = gpios[i];
    cfg.flags.fb_in_psram         = 1;

    esp_err_t ret = esp_lcd_new_rgb_panel(&cfg, &s_panel);
    if (ret != ESP_OK) fail_report(1, ret);   // 1 LONG + error
    esp_lcd_panel_reset(s_panel);
    ret = esp_lcd_panel_init(s_panel);
    if (ret != ESP_OK) fail_report(2, ret);   // 2 LONG + error

    // Our own working buffer in PSRAM; push via esp_lcd_panel_draw_bitmap (which
    // flushes the PSRAM cache before DMA) — exactly as ESPHome's draw_pixels_at.
    // Direct framebuffer writes leave stale cache → blank panel even if init OK.
    s_fb = (uint16_t *)heap_caps_malloc(480u * 640u * 2u, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_fb) fail_report(3, ESP_ERR_NO_MEM); // 3 LONG + error

    beep(3);  // ✓ panel + buffer OK — then colour sweep, then quiet

    // Stage-A colour sweep — draw_bitmap (NOT direct fb writes)
    uint32_t n = 480u * 640u;
    for (uint32_t i = 0; i < n; i++) s_fb[i] = 0xF800; esp_lcd_panel_draw_bitmap(s_panel,0,0,480,640,s_fb); delay(600);
    for (uint32_t i = 0; i < n; i++) s_fb[i] = 0x07E0; esp_lcd_panel_draw_bitmap(s_panel,0,0,480,640,s_fb); delay(600);
    for (uint32_t i = 0; i < n; i++) s_fb[i] = 0x001F; esp_lcd_panel_draw_bitmap(s_panel,0,0,480,640,s_fb); delay(600);
    for (uint32_t i = 0; i < n; i++) s_fb[i] = 0x0000; esp_lcd_panel_draw_bitmap(s_panel,0,0,480,640,s_fb);
}

// ── push() — called by buddyPush() ───────────────────────────────────────────
static void push(const uint16_t *buf) {
    if (!s_panel || !buf) return;
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, 480, 640, buf);  // flushes cache + DMAs
}

} // namespace ws2_8b

#endif // BOARD_WAVESHARE_2_8B
