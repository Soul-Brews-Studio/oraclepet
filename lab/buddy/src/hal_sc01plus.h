#pragma once
// HAL for WT32-SC01 Plus (ESP32-S3 + ST7796UI 8-bit 8080 parallel + FT6336U touch)
// Same 320×480 resolution as ATD3.5-S3 / JC3248 — reuses the BOARD_ATD35_S3
// HalM5 wrapper + stubs in hal.h (this board's env also defines BOARD_ATD35_S3).
// LGFX device by esp32-exp (lgfx_sc01plus.h); folded into the buddy HAL pattern.

#if defined(BOARD_WT32_SC01_PLUS)

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

class LGFX_SC01Plus : public lgfx::LGFX_Device {
  lgfx::Panel_ST7796   _panel;   // ST7796UI controller
  lgfx::Bus_Parallel8  _bus;     // 8-bit Intel-8080 parallel (LCD_CAM on S3)
  lgfx::Light_PWM      _light;   // backlight
  lgfx::Touch_FT5x06   _touch;   // FT6336U is FT5x06-protocol-compatible

public:
  LGFX_SC01Plus() {
    // ── 8080 8-bit parallel bus ──────────────────────────────────────
    {
      auto cfg = _bus.config();
      cfg.port        = 0;            // ESP32-S3 LCD_CAM peripheral
      cfg.freq_write  = 40000000;     // VERIFY: drop to 20MHz if tearing/garbage
      cfg.pin_wr      = 47;           // write strobe
      cfg.pin_rd      = -1;           // RD not broken out (write-only display)
      cfg.pin_rs      = 0;            // DC/RS — GPIO0 boot strap; safe after boot
      cfg.pin_d0 = 9;
      cfg.pin_d1 = 46;                // GPIO46 strapping pin (data only)
      cfg.pin_d2 = 3;
      cfg.pin_d3 = 8;
      cfg.pin_d4 = 18;
      cfg.pin_d5 = 17;
      cfg.pin_d6 = 16;
      cfg.pin_d7 = 15;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    // ── ST7796 panel ─────────────────────────────────────────────────
    {
      auto cfg = _panel.config();
      cfg.pin_cs        = -1;         // CS tied active on the SC01 Plus
      cfg.pin_rst       = 4;
      cfg.pin_busy      = -1;
      cfg.memory_width  = 320;
      cfg.memory_height = 480;
      cfg.panel_width   = 320;
      cfg.panel_height  = 480;
      cfg.offset_x      = 0;
      cfg.offset_y      = 0;
      cfg.offset_rotation = 0;
      cfg.readable      = false;      // no RD line
      cfg.invert        = true;       // VERIFY on panel
      cfg.rgb_order     = false;      // false = BGR. VERIFY (red/blue swap)
      cfg.dlen_16bit    = false;      // 8-bit bus
      cfg.bus_shared    = false;
      _panel.config(cfg);
    }
    // ── Backlight (GPIO45, PWM) ──────────────────────────────────────
    {
      auto cfg = _light.config();
      cfg.pin_bl      = 45;
      cfg.freq        = 5000;
      cfg.pwm_channel = 7;
      cfg.invert      = false;
      _light.config(cfg);
      _panel.setLight(&_light);
    }
    // ── FT6336U capacitive touch (I²C) ───────────────────────────────
    {
      auto cfg = _touch.config();
      cfg.i2c_port   = 0;
      cfg.pin_sda    = 6;
      cfg.pin_scl    = 5;
      cfg.pin_int    = 7;
      cfg.pin_rst    = -1;
      cfg.i2c_addr   = 0x38;          // FT6336U
      cfg.freq       = 400000;
      cfg.x_min      = 0;   cfg.x_max = 319;
      cfg.y_min      = 0;   cfg.y_max = 479;
      cfg.bus_shared = false;
      _touch.config(cfg);
      _panel.setTouch(&_touch);
    }
    setPanel(&_panel);
  }
};

extern LGFX_SC01Plus tft;
using LGFX_ATD35 = LGFX_SC01Plus;   // HalM5 wrapper (hal.h) is written against LGFX_ATD35

#endif
