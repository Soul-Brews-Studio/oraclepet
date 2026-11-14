#pragma once

// Hardware Abstraction Layer
// Selects between M5StickC Plus (original), ATD3.5-S3, JC3248, LilyGo EPD47,
// and Waveshare ESP32-S3-LCD-2.8B (BOARD_WAVESHARE_2_8B).

#if defined(BOARD_LILYGO_EPD47)

#include "hal_epd47.h"

#elif defined(BOARD_C3_HMI)

#include "hal_c3hmi.h"

#elif defined(BOARD_JC3248W535C)

#include "hal_jc3248.h"

#elif defined(BOARD_WT32_SC01_PLUS)

#include "hal_sc01plus.h"

#elif defined(BOARD_WAVESHARE_2_8B)

#include "hal_ws2_8b.h"

#elif defined(BOARD_ATD35_S3)

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

// ATD3.5-S3 pin definitions (from ArtronShop schematic)
#define ATD_SPI_SCK  12
#define ATD_SPI_MOSI 11
#define ATD_SPI_MISO 13
#define ATD_LCD_CS   10
#define ATD_LCD_DC   21
#define ATD_LCD_RST  14
#define ATD_LCD_BL   3
#define ATD_TOUCH_SDA 15
#define ATD_TOUCH_SCL 16

class LGFX_ATD35 : public lgfx::LGFX_Device {
  lgfx::Panel_ST7796 _panel;
  lgfx::Bus_SPI _bus;
  lgfx::Light_PWM _light;
  lgfx::Touch_FT5x06 _touch;

public:
  LGFX_ATD35() {
    {
      auto cfg = _bus.config();
      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 40000000;
      cfg.freq_read  = 16000000;
      cfg.pin_sclk = ATD_SPI_SCK;
      cfg.pin_mosi = ATD_SPI_MOSI;
      cfg.pin_miso = ATD_SPI_MISO;
      cfg.pin_dc   = ATD_LCD_DC;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs   = ATD_LCD_CS;
      cfg.pin_rst  = ATD_LCD_RST;
      cfg.pin_busy = -1;
      cfg.memory_width  = 320;
      cfg.memory_height = 480;
      cfg.panel_width   = 320;
      cfg.panel_height  = 480;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.offset_rotation = 0;
      cfg.invert = true;
      cfg.rgb_order = false;
      _panel.config(cfg);
    }
    {
      auto cfg = _light.config();
      cfg.pin_bl = ATD_LCD_BL;
      cfg.invert = false;
      cfg.freq = 44100;
      cfg.pwm_channel = 7;
      _light.config(cfg);
      _panel.setLight(&_light);
    }
    {
      auto cfg = _touch.config();
      cfg.i2c_port = 1;
      cfg.i2c_addr = 0x38;
      cfg.pin_sda = ATD_TOUCH_SDA;
      cfg.pin_scl = ATD_TOUCH_SCL;
      cfg.freq = 400000;
      cfg.x_min = 0;
      cfg.x_max = 319;
      cfg.y_min = 0;
      cfg.y_max = 479;
      _touch.config(cfg);
      _panel.setTouch(&_touch);
    }
    setPanel(&_panel);
  }
};

#if defined(BOARD_WAVESHARE_2_8B)
extern LGFX_WS2_8B tft;
#else
extern LGFX_ATD35 tft;
#endif

#endif // display-specific (JC3248 or ATD35)

#if defined(BOARD_ATD35_S3)
// Shared stubs for all LovyanGFX-based boards (ATD35, JC3248, etc.)

// Compatibility aliases — use LGFX_Device as base so LGFX_ATD35* converts
using TFT_eSPI = lgfx::LGFX_Device;
class TFT_eSprite : public LGFX_Sprite {
public:
  TFT_eSprite() : LGFX_Sprite() { _psram = true; }
  TFT_eSprite(LovyanGFX* parent) : LGFX_Sprite(parent) { _psram = true; }
  void* frameBuffer(uint8_t) { return getBuffer(); }
};

// Display dimensions
#if defined(BOARD_WAVESHARE_2_8B)
static const int HAL_SCREEN_W = 480;
static const int HAL_SCREEN_H = 640;
#else
static const int HAL_SCREEN_W = 320;
static const int HAL_SCREEN_H = 480;
#endif

// Color constants
#ifndef GREEN
#define GREEN 0x07E0
#endif
#ifndef RED
#define RED 0xF800
#endif

// Text datum compatibility
#ifndef MC_DATUM
#define MC_DATUM lgfx::textdatum::middle_center
#endif
#ifndef TL_DATUM
#define TL_DATUM lgfx::textdatum::top_left
#endif

// Stub: RTC time types
struct RTC_TimeTypeDef { uint8_t Hours = 0, Minutes = 0, Seconds = 0; };
struct RTC_DateTypeDef { uint8_t WeekDay = 0, Month = 1, Date = 1; uint16_t Year = 2026; };

// Stub: IMU
struct HalImu {
  void Init() {}
  void getAccelData(float* ax, float* ay, float* az) {
    *ax = 0; *ay = -1.0f; *az = 0;
  }
};

// Stub: AXP power
struct HalAxp {
  uint8_t _brightness = 100;
  void ScreenBreath(int v) {
    _brightness = constrain(v, 0, 100);
    tft.setBrightness(map(_brightness, 0, 100, 0, 255));
  }
  void SetLDO2(bool on) {
    tft.setBrightness(on ? map(_brightness, 0, 100, 0, 255) : 0);
  }
  void PowerOff() { esp_deep_sleep_start(); }
  float GetBatVoltage() { return 4.2f; }
  float GetBatCurrent() { return 0; }
  float GetVBusVoltage() { return 5.0f; }
  float GetTempInAXP192() { return 25.0f; }
  uint8_t GetBtnPress() { return 0; }
};

// Stub: RTC
struct HalRtc {
  RTC_TimeTypeDef _t;
  RTC_DateTypeDef _d;
  void SetTime(RTC_TimeTypeDef* t) { _t = *t; }
  void SetDate(RTC_DateTypeDef* d) { _d = *d; }
  void GetTime(RTC_TimeTypeDef* t) { *t = _t; }
  void GetDate(RTC_DateTypeDef* d) { *d = _d; }
};

// Stub: Beep
struct HalBeep {
  void begin() {}
  void update() {}
  void tone(uint16_t, uint16_t) {}
};

// Button emulation from touch
struct HalButton {
  bool _pressed = false;
  bool _wasPressed = false;
  bool _wasReleased = false;
  uint32_t _pressedSince = 0;

  void update(bool state) {
    bool prev = _pressed;
    _pressed = state;
    _wasPressed = !prev && state;
    _wasReleased = prev && !state;
    if (!prev && state) _pressedSince = millis();
    if (!state) _pressedSince = 0;
  }
  bool isPressed() { return _pressed; }
  bool wasPressed() { bool r = _wasPressed; _wasPressed = false; return r; }
  bool wasReleased() { bool r = _wasReleased; _wasReleased = false; return r; }
  bool pressedFor(uint32_t ms) {
    return _pressed && _pressedSince && (millis() - _pressedSince >= ms);
  }
};

// Touch gesture recognizer
struct TouchState {
  enum Gesture { NONE, TAP, LONG_PRESS, SWIPE_LEFT, SWIPE_RIGHT, SWIPE_UP, SWIPE_DOWN };
  bool touching = false;
  int16_t x = 0, y = 0;
  int16_t startX = 0, startY = 0;
  uint32_t startMs = 0;
  bool gestureEmitted = false;
  Gesture gesture = NONE;
  int16_t tapX = 0, tapY = 0;

  void beginTouch(int16_t tx, int16_t ty) {
    touching = true;
    x = startX = tx;
    y = startY = ty;
    startMs = millis();
    gestureEmitted = false;
  }
  void moveTouch(int16_t tx, int16_t ty) {
    x = tx; y = ty;
  }
  void endTouch() {
    if (!gestureEmitted) {
      int16_t dx = x - startX, dy = y - startY;
      uint32_t dur = millis() - startMs;
      int16_t adx = abs(dx), ady = abs(dy);
      if (adx < 15 && ady < 15 && dur < 300) {
        gesture = TAP; tapX = x; tapY = y;
      } else if (adx > 40 && adx > ady && dur < 500) {
        gesture = dx < 0 ? SWIPE_LEFT : SWIPE_RIGHT; tapX = x; tapY = y;
      } else if (ady > 40 && ady > adx && dur < 500) {
        gesture = dy < 0 ? SWIPE_UP : SWIPE_DOWN; tapX = x; tapY = y;
      }
    }
    touching = false;
  }
  void checkLongPress() {
    if (touching && !gestureEmitted) {
      int16_t adx = abs(x - startX), ady = abs(y - startY);
      if (adx < 15 && ady < 15 && millis() - startMs > 600) {
        gesture = LONG_PRESS; tapX = x; tapY = y;
        gestureEmitted = true;
      }
    }
  }
  void consume() { gesture = NONE; }
};

// M5-like wrapper struct
struct HalM5 {
  HalImu Imu;
  HalAxp Axp;
  HalRtc Rtc;
  HalBeep Beep;
  HalButton BtnA;
  HalButton BtnB;
  TouchState Touch;
  lgfx::LGFX_Device& Lcd;

  HalM5(lgfx::LGFX_Device& lcd) : Lcd(lcd) {}

  void begin() {
#if defined(BOARD_JC3248W535C)
    // JC3248 AXS15231 is DC-less QSPI-DBI: LovyanGFX can't render it, so we do
    // NOT init the LovyanGFX panel/bus (would grab SPI2). The native axs driver
    // owns SPI2 and pushes the sprite. Lcd stays a draw-only sprite parent.
    axs::init();
#elif defined(BOARD_WAVESHARE_2_8B)
    // 2.8B: ST7701S+TCA9554+RGB — ws2_8b::init() owns the hardware, LGFX device
    // is sprite-parent only (no bus to init).
    ws2_8b::init();
#else
    Lcd.init();
    Lcd.setRotation(0);
    Lcd.setBrightness(200);
    Lcd.fillScreen(0x0000);
#endif
  }

  void update() {
    lgfx::touch_point_t tp;
#if defined(BOARD_JC3248W535C)
    // JC3248 touch is read natively (LovyanGFX touch never inits — see begin()).
    int16_t _tx = 0, _ty = 0;
    int touched = axs::read_touch(&_tx, &_ty) ? 1 : 0;
    tp.x = _tx; tp.y = _ty;
#elif defined(BOARD_WAVESHARE_2_8B)
    // 2.8B has no touch panel.
    int touched = 0;
    tp.x = 0; tp.y = 0;
#else
    int touched = Lcd.getTouch(&tp);
#endif

    if (touched > 0) {
      if (!Touch.touching) Touch.beginTouch(tp.x, tp.y);
      else Touch.moveTouch(tp.x, tp.y);
    } else {
      if (Touch.touching) Touch.endTouch();
    }
    Touch.checkLongPress();

    // Button emulation from bottom 80px
    bool touchA = false, touchB = false;
    if (touched > 0 && tp.y > HAL_SCREEN_H - 80) {
      if (tp.x < HAL_SCREEN_W / 2) touchA = true;
      else touchB = true;
    }
    BtnA.update(touchA);
    BtnB.update(touchB);
  }
};

extern HalM5 M5;

// Push the rendered sprite to the panel. JC3248 (AXS15231 QSPI-DBI) goes through
// the native axs driver; every other board uses LovyanGFX's native pushSprite.
static inline void buddyPush(TFT_eSprite& s) {
#if defined(BOARD_JC3248W535C)
  axs::push(reinterpret_cast<const uint16_t*>(s.getBuffer()));
#elif defined(BOARD_WAVESHARE_2_8B)
  ws2_8b::push(reinterpret_cast<const uint16_t*>(s.getBuffer()));
#else
  s.pushSprite(0, 0);
#endif
}

#elif !defined(BOARD_LILYGO_EPD47)
// Original M5StickC Plus — no changes needed
#include <M5StickCPlus.h>
#endif
