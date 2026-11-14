#pragma once
// LGFX_JC3248 — the LovyanGFX device for the JC3248W535 (from the buddy's
// hal_jc3248.h). Apps that probe tft.* interactions (004 parented sprite,
// 005 setRotation, 006 setBrightness) instantiate this locally to see whether
// touching a LovyanGFX device on SPI2 disrupts the native axs driver.
// Requires: -DLGFX_USE_V1 -DLGFX_USE_QSPI and #include <LovyanGFX.hpp> first.
#include <LovyanGFX.hpp>

#define JC_QSPI_CLK  47
#define JC_QSPI_D0   21
#define JC_QSPI_D1   48
#define JC_QSPI_D2   40
#define JC_QSPI_D3   39
#define JC_LCD_CS    45
#define JC_LCD_BL    1

// AXS15231 panel on LovyanGFX's QSPI-DBI base (Panel_AMOLED frames 0x02/0x32).
class Panel_AXS15231_dev : public lgfx::Panel_AMOLED {
public:
  Panel_AXS15231_dev() {
    _cfg.memory_width  = _cfg.panel_width  = 320;
    _cfg.memory_height = _cfg.panel_height = 480;
    _write_depth = lgfx::color_depth_t::rgb565_2Byte;
    _read_depth  = lgfx::color_depth_t::rgb565_2Byte;
  }
  const uint8_t* getInitCommands(uint8_t listno) const override {
    static constexpr uint8_t list0[] = {
      0xBB, 8, 0x00,0x00,0x00,0x00,0x00,0x00,0x5A,0xA5,
      0xC1, 1, 0x33,
      0xBB, 8, 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
      0x11, 0 + CMD_INIT_DELAY, 120,
      0x3A, 1, 0x55,
      0x36, 1, 0x00,
      0x29, 0 + CMD_INIT_DELAY, 20,
      0xFF, 0xFF
    };
    return listno == 0 ? list0 : nullptr;
  }
};

class LGFX_JC3248 : public lgfx::LGFX_Device {
  Panel_AXS15231_dev _panel;
  lgfx::Bus_SPI      _bus;
  lgfx::Light_PWM    _light;
public:
  LGFX_JC3248() {
    { auto c = _bus.config();
      c.spi_host = SPI2_HOST; c.spi_mode = 0;
      c.freq_write = 40000000; c.freq_read = 16000000;
      c.pin_sclk = JC_QSPI_CLK; c.pin_io0 = JC_QSPI_D0; c.pin_io1 = JC_QSPI_D1;
      c.pin_io2 = JC_QSPI_D2; c.pin_io3 = JC_QSPI_D3; c.pin_dc = -1;
      _bus.config(c); _panel.setBus(&_bus); }
    { auto c = _panel.config();
      c.pin_cs = JC_LCD_CS; c.pin_rst = -1; c.pin_busy = -1; c.readable = false;
      _panel.config(c); }
    { auto c = _light.config();
      c.pin_bl = JC_LCD_BL; c.invert = false; c.freq = 44100; c.pwm_channel = 7;
      _light.config(c); _panel.setLight(&_light); }
    setPanel(&_panel);
  }
};
