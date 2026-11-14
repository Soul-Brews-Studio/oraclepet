// ═══════════════════════════════════════════════════════════════════════
//  lgfx_sc01plus.h — LovyanGFX display init for the WT32-SC01 Plus
//
//  Buddy-port display target: Guition/Panlee WT32-SC01 Plus
//    ESP32-S3 (16MB flash + 2MB PSRAM) · ST7796UI 320×480 · 8-bit 8080
//    parallel bus · FT6336U capacitive touch (I²C).
//
//  WHY LovyanGFX (not ESPHome): ESPHome has NO 8080/i80 parallel display
//  bus — the io_bus/i80 line (PRs #8227–8230) was reverted (#8384) and
//  abandoned; successor `mipi_spi` is SPI/QSPI only (open FR #2533). The
//  SC01 Plus screen is driven via LovyanGFX `Bus_Parallel8` + `Panel_ST7796`.
//
//  Pin map is the single source of truth verified 2026-05-29 (panel-tag docs
//  + LovyanGFX/TFT_eSPI community configs). See lab/wt32-sc01-plus/README.md.
//
//  ── How this slots into the buddy-port ────────────────────────────────
//  The existing code is written against TFT_eSPI:
//      TFT_eSprite spr = TFT_eSprite(&M5.Lcd);          // main.cpp
//      void characterRenderTo(TFT_eSPI* tgt, ...);      // character.h
//  LovyanGFX's LGFX / LGFX_Sprite expose the SAME drawing API (fillSprite,
//  pushSprite, drawString, setTextDatum, fillRect, setRotation, …). Port via
//  a compat shim (see buddy_lgfx_compat.h sketch in the PR notes):
//      using TFT_eSPI    = lgfx::LovyanGFX;     // common base for display + sprite ptrs
//      using TFT_eSprite = lgfx::LGFX_Sprite;
//      LGFX_SC01Plus Lcd;                        // replaces M5.Lcd
//      TFT_eSprite   spr(&Lcd);
//  The 45 M5.* hardware calls (Axp power, Imu, Rtc, Beep, Btn) have NO
//  SC01 Plus equivalent — stub or gate them (the SC01 Plus has no AXP192
//  PMIC, no IMU, no RTC). That's the bulk of the remaining port; this header
//  is just the screen+touch bring-up.
//
//  ⚠️ FOUR values to confirm on the real panel (commented inline below):
//     freq_write, invert, rgb_order, touch transform/rotation.
// ═══════════════════════════════════════════════════════════════════════
#pragma once

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
      cfg.freq_write  = 40000000;     // 40MHz. VERIFY: drop to 20MHz if the
                                      //   panel shows tearing/garbage.
      cfg.pin_wr      = 47;           // write strobe
      cfg.pin_rd      = -1;           // RD not broken out (write-only display)
      cfg.pin_rs      = 0;            // DC/RS — GPIO0 is the boot strap pin;
                                      //   safe to drive as DC after boot.
      cfg.pin_d0 = 9;
      cfg.pin_d1 = 46;                // GPIO46 is a strapping pin (data only)
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
      cfg.memory_width  = 320;        // controller RAM
      cfg.memory_height = 480;
      cfg.panel_width   = 320;        // visible
      cfg.panel_height  = 480;
      cfg.offset_x      = 0;
      cfg.offset_y      = 0;
      cfg.offset_rotation = 0;
      cfg.readable      = false;      // no RD line
      cfg.invert        = true;       // VERIFY: ST7796 on SC01 Plus usually
                                      //   needs invert; flip if image is negative.
      cfg.rgb_order     = false;      // false = BGR. VERIFY: set true if red/blue swap.
      cfg.dlen_16bit    = false;      // 8-bit bus
      cfg.bus_shared    = false;      // bus is display-only
      _panel.config(cfg);
    }

    // ── Backlight (GPIO45, PWM) ──────────────────────────────────────
    {
      auto cfg = _light.config();
      cfg.pin_bl      = 45;           // GPIO45 (strapping pin; LEDC ok after boot)
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
      cfg.pin_int    = 7;             // INT (from HA-community SC01 Plus config)
      cfg.pin_rst    = -1;            // not broken out
      cfg.i2c_addr   = 0x38;          // FT6336U
      cfg.freq       = 400000;
      cfg.x_min      = 0;   cfg.x_max = 319;
      cfg.y_min      = 0;   cfg.y_max = 479;
      cfg.bus_shared = false;
      // VERIFY orientation against the panel: the FT6336 axes often need a
      // swap/mirror relative to the ST7796 — adjust offset_rotation above
      // and/or these min/max once you can see touch coords.
      _touch.config(cfg);
      _panel.setTouch(&_touch);
    }

    setPanel(&_panel);
  }
};
