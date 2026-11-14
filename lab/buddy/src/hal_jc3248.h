#pragma once
// HAL for JC3248W535C (ESP32-S3 + AXS15231B QSPI display + AXS15231 touch)
//
// The AXS15231 over QSPI is a DC-less DBI panel: commands framed as
// [0x02,0x00,cmd,0x00], pixels as [0x32,0x00,0x2C,0x00]+quad-data. LovyanGFX's
// generic Panel_LCD/Bus_SPI does NOT emit that framing (it assumes a DC pin),
// so the panel never rendered — it kept the prior firmware's frame.
// Fix: drive it via lgfx::Panel_AMOLED, the QSPI-DBI base (RM67162/SH8601Z
// family) that sends exactly the 0x02/0x32 prefixes. Verified vs ESPHome
// mipi_spi (which works on this board). Needs -DLGFX_USE_QSPI.

#if defined(BOARD_JC3248W535C)

#define LGFX_USE_V1
#define LGFX_USE_QSPI
#include <LovyanGFX.hpp>

// JC3248W535C pin definitions
#define JC_QSPI_CLK  47
#define JC_QSPI_D0   21
#define JC_QSPI_D1   48
#define JC_QSPI_D2   40
#define JC_QSPI_D3   39
#define JC_LCD_CS     45
#define JC_LCD_BL     1
#define JC_TOUCH_SDA  4
#define JC_TOUCH_SCL  8

// AXS15231 touch controller — custom I2C protocol (not FT5x06 compatible)
struct Touch_AXS15231 : public lgfx::ITouch {
  Touch_AXS15231() {
    _cfg.i2c_addr = 0x3B;
    _cfg.x_min = 0; _cfg.x_max = 319;
    _cfg.y_min = 0; _cfg.y_max = 479;
    _cfg.freq  = 400000;
  }
  bool init() override {
    _inited = false;
    if (lgfx::i2c::init(_cfg.i2c_port, _cfg.pin_sda, _cfg.pin_scl).has_value()) _inited = true;
    return _inited;
  }
  void wakeup() override {}
  void sleep() override {}
  uint_fast8_t getTouchRaw(lgfx::touch_point_t* tp, uint_fast8_t count) override {
    if (!_inited || count == 0) return 0;
    static constexpr uint8_t AXS_READ_CMD[11] = { 0xb5, 0xab, 0xa5, 0x5a, 0x0, 0x0, 0x0, 0x8 };
    uint8_t data[8] = {};
    if (!lgfx::i2c::transactionWriteRead(
          _cfg.i2c_port, _cfg.i2c_addr, AXS_READ_CMD, sizeof(AXS_READ_CMD),
          data, sizeof(data), _cfg.freq).has_value()) return 0;
    if (data[0] != 0 || data[1] == 0) return 0;
    tp[0].x = ((data[2] & 0x0F) << 8) | data[3];
    tp[0].y = ((data[4] & 0x0F) << 8) | data[5];
    tp[0].size = 1; tp[0].id = 0;
    return 1;
  }
};

// AXS15231 panel on the QSPI-DBI base (Panel_AMOLED frames 0x02/0x32 itself).
class Panel_AXS15231 : public lgfx::Panel_AMOLED {
public:
  Panel_AXS15231() {
    _cfg.memory_width  = _cfg.panel_width  = 320;
    _cfg.memory_height = _cfg.panel_height = 480;
    _write_depth = lgfx::color_depth_t::rgb565_2Byte;
    _read_depth  = lgfx::color_depth_t::rgb565_2Byte;
  }
  const uint8_t* getInitCommands(uint8_t listno) const override {
    // AXS15231 vendor unlock (matches ESPHome's working AXS15231 model
    // byte-for-byte) + standard MIPI sleep-out / COLMOD / MADCTL / display-on.
    static constexpr uint8_t list0[] = {
      0xBB, 8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5A, 0xA5,
      0xC1, 1, 0x33,
      0xBB, 8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x11, 0 + CMD_INIT_DELAY, 120,   // sleep out + 120ms
      0x3A, 1, 0x55,                   // COLMOD = RGB565
      0x36, 1, 0x00,                   // MADCTL = RGB, no flip (ESPHome=RGB) — VERIFY
      0x29, 0 + CMD_INIT_DELAY, 20,    // display on + 20ms
      0xFF, 0xFF                       // end
    };
    return listno == 0 ? list0 : nullptr;
  }
};

class LGFX_JC3248 : public lgfx::LGFX_Device {
  Panel_AXS15231 _panel;
  lgfx::Bus_SPI _bus;
  lgfx::Light_PWM _light;
  Touch_AXS15231 _touch;
public:
  LGFX_JC3248() {
    {
      auto cfg = _bus.config();
      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 40000000;
      cfg.freq_read  = 16000000;
      cfg.pin_sclk = JC_QSPI_CLK;
      cfg.pin_io0  = JC_QSPI_D0;
      cfg.pin_io1  = JC_QSPI_D1;
      cfg.pin_io2  = JC_QSPI_D2;
      cfg.pin_io3  = JC_QSPI_D3;
      cfg.pin_dc   = -1;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs   = JC_LCD_CS;
      cfg.pin_rst  = -1;
      cfg.pin_busy = -1;
      cfg.readable = false;
      _panel.config(cfg);
    }
    {
      auto cfg = _light.config();
      cfg.pin_bl = JC_LCD_BL;
      cfg.invert = false;
      cfg.freq = 44100;
      cfg.pwm_channel = 7;
      _light.config(cfg);
      _panel.setLight(&_light);
    }
    {
      auto cfg = _touch.config();
      cfg.i2c_port = 0;
      cfg.i2c_addr = 0x3B;
      cfg.pin_sda = JC_TOUCH_SDA;
      cfg.pin_scl = JC_TOUCH_SCL;
      cfg.freq = 400000;
      cfg.x_min = 0; cfg.x_max = 319;
      cfg.y_min = 0; cfg.y_max = 479;
      _touch.config(cfg);
      _panel.setTouch(&_touch);
    }
    setPanel(&_panel);
  }
};

extern LGFX_JC3248 tft;
using LGFX_ATD35 = LGFX_JC3248;   // HalM5 wrapper (hal.h) is written against LGFX_ATD35

// Native AXS15231 QSPI push driver — LovyanGFX draws the sprite, this pushes it.
// (LovyanGFX can't render this DC-less QSPI-DBI panel; see the file header.)
#include "axs15231_qspi.h"

#endif
