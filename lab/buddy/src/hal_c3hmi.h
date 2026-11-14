#pragma once
// HAL for MakerAsia C3-HMI (ESP32-C3 + ST7789 SPI 240×320 + FT5x06 touch)
// Pin mapping from board silkscreen: SCK=4, SDI=5, CSX=7, DCX=10,
// SDA=8, SCL=9, BLK=18, INT=3.
// ESP32-C3 is single-core RISC-V, 4MB flash, no PSRAM.
// Display is portrait 240×320; buddy renders at 135×240 centered or scaled.

#if defined(BOARD_C3_HMI)

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

class LGFX_C3HMI : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789  _panel;
  lgfx::Bus_SPI       _bus;
  lgfx::Light_PWM     _light;
  lgfx::Touch_FT5x06  _touch;

public:
  LGFX_C3HMI() {
    // ── SPI bus ──────────────────────────────────────────────────────
    {
      auto cfg = _bus.config();
      cfg.spi_host   = SPI2_HOST;
      cfg.spi_mode   = 0;
      cfg.freq_write = 40000000;
      cfg.freq_read  = 16000000;
      cfg.pin_sclk   = 4;
      cfg.pin_mosi   = 5;
      cfg.pin_miso   = -1;
      cfg.pin_dc     = 10;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    // ── ST7789 panel ─────────────────────────────────────────────────
    {
      auto cfg = _panel.config();
      cfg.pin_cs        = 7;
      cfg.pin_rst       = -1;
      cfg.pin_busy      = -1;
      cfg.memory_width  = 240;
      cfg.memory_height = 320;
      cfg.panel_width   = 240;
      cfg.panel_height  = 320;
      cfg.offset_x      = 0;
      cfg.offset_y      = 0;
      cfg.offset_rotation = 0;
      cfg.invert        = true;
      cfg.rgb_order     = false;
      _panel.config(cfg);
    }
    // ── Backlight (GPIO18, PWM) ──────────────────────────────────────
    {
      auto cfg = _light.config();
      cfg.pin_bl      = 18;
      cfg.freq        = 5000;
      cfg.pwm_channel = 0;
      cfg.invert      = false;
      _light.config(cfg);
      _panel.setLight(&_light);
    }
    // ── FT5x06 capacitive touch (I²C) ────────────────────────────────
    {
      auto cfg = _touch.config();
      cfg.i2c_port   = 0;
      cfg.pin_sda    = 8;
      cfg.pin_scl    = 9;
      cfg.pin_int    = 3;
      cfg.pin_rst    = -1;
      cfg.i2c_addr   = 0x38;
      cfg.freq       = 400000;
      cfg.x_min      = 0;   cfg.x_max = 239;
      cfg.y_min      = 0;   cfg.y_max = 319;
      cfg.bus_shared = false;
      _touch.config(cfg);
      _panel.setTouch(&_touch);
    }
    setPanel(&_panel);
  }
};

extern LGFX_C3HMI tft;
using LGFX_ATD35 = LGFX_C3HMI;

#endif
