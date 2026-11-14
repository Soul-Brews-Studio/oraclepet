#pragma once
#if defined(BOARD_LILYGO_EPD47)

#include <Arduino.h>
#include "epd_driver.h"
#include "epd_highlevel.h"
#include <esp_heap_caps.h>

// LilyGo T5 4.7" V2.x (ESP32 WROVER-E + ED047TC1)
// 960×540, 16-level grayscale, parallel GPIO drive via epdiy
// 3 physical buttons, no touch, no backlight, no IMU

static const int HAL_SCREEN_W = 960;
static const int HAL_SCREEN_H = 540;

// Button GPIOs (LilyGo T5 4.7" V2.x)
#define EPD_BTN1 21
#define EPD_BTN2 34
#define EPD_BTN3 39

// RGB565 → 4-bit grayscale (0=black, 15=white)
inline uint8_t rgb565ToGray4(uint16_t c) {
  uint8_t r = (c >> 11) & 0x1F;
  uint8_t g = (c >> 5) & 0x3F;
  uint8_t b = c & 0x1F;
  // luminance: 0.299R + 0.587G + 0.114B, scaled for 5/6/5 bit depths
  uint16_t lum = (r * 19 + g * 9 + b * 19) / 32;
  return (uint8_t)(lum & 0x0F);
}

// Minimal TFT_eSprite-compatible wrapper around epdiy high-level API.
// Uses epd_hl for framebuffer management and screen updates.
// Framebuffer is 4bpp packed (2 pixels per byte, upper nibble = left pixel).
class EpdSprite {
public:
  uint8_t* fb = nullptr;
  uint8_t* prevFb = nullptr;
  EpdiyHighlevelState _hl;
  int _w = HAL_SCREEN_W;
  int _h = HAL_SCREEN_H;
  int _curX = 0, _curY = 0;
  uint8_t _fg = 0;   // 4-bit foreground
  uint8_t _bg = 15;  // 4-bit background
  uint8_t _textSize = 1;
  uint8_t _datum = 0; // 0=TL, 1=MC
  bool _dirty = true;
  uint32_t _lastFullRefresh = 0;
  static const uint32_t FULL_REFRESH_INTERVAL = 300000; // 5 min

  EpdSprite() {}
  EpdSprite(void*) {} // compat with TFT_eSprite(&tft) pattern

  void* createSprite(int w, int h) {
    _w = w; _h = h;
    size_t sz = (w * h) / 2; // 4bpp packed
    fb = (uint8_t*)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
    prevFb = (uint8_t*)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
    if (fb) memset(fb, 0xFF, sz);
    if (prevFb) memset(prevFb, 0xFF, sz);
    return fb;
  }

  void* frameBuffer(uint8_t) { return fb; }
  void* getBuffer() { return fb; }

  void fillSprite(uint16_t color) {
    uint8_t g = rgb565ToGray4(color);
    uint8_t packed = (g << 4) | g;
    memset(fb, packed, (_w * _h) / 2);
    _dirty = true;
  }

  void fillRect(int x, int y, int w, int h, uint16_t color) {
    uint8_t g = rgb565ToGray4(color);
    for (int py = y; py < y + h && py < _h; py++) {
      for (int px = x; px < x + w && px < _w; px++) {
        _setPixel(px, py, g);
      }
    }
    _dirty = true;
  }

  void fillRoundRect(int x, int y, int w, int h, int r, uint16_t color) {
    fillRect(x, y, w, h, color);
  }

  void drawRoundRect(int x, int y, int w, int h, int r, uint16_t color) {
    drawRect(x, y, w, h, color);
  }

  void drawRect(int x, int y, int w, int h, uint16_t color) {
    drawFastHLine(x, y, w, color);
    drawFastHLine(x, y + h - 1, w, color);
    for (int py = y; py < y + h; py++) {
      _setPixel(x, py, rgb565ToGray4(color));
      _setPixel(x + w - 1, py, rgb565ToGray4(color));
    }
    _dirty = true;
  }

  void drawFastHLine(int x, int y, int w, uint16_t color) {
    uint8_t g = rgb565ToGray4(color);
    for (int px = x; px < x + w && px < _w; px++) _setPixel(px, y, g);
    _dirty = true;
  }

  void fillCircle(int x, int y, int r, uint16_t color) {
    uint8_t g = rgb565ToGray4(color);
    for (int dy = -r; dy <= r; dy++) {
      for (int dx = -r; dx <= r; dx++) {
        if (dx*dx + dy*dy <= r*r) _setPixel(x+dx, y+dy, g);
      }
    }
    _dirty = true;
  }

  void drawCircle(int x, int y, int r, uint16_t color) {
    uint8_t g = rgb565ToGray4(color);
    for (int a = 0; a < 360; a++) {
      int px = x + (int)(r * cos(a * 3.14159 / 180));
      int py = y + (int)(r * sin(a * 3.14159 / 180));
      _setPixel(px, py, g);
    }
    _dirty = true;
  }

  void fillTriangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t color) {
    // Simple scanline fill
    uint8_t g = rgb565ToGray4(color);
    int minY = min(y0, min(y1, y2));
    int maxY = max(y0, max(y1, y2));
    for (int y = minY; y <= maxY && y < _h; y++) {
      int minX = _w, maxX = 0;
      _triEdge(x0,y0,x1,y1, y, minX, maxX);
      _triEdge(x1,y1,x2,y2, y, minX, maxX);
      _triEdge(x2,y2,x0,y0, y, minX, maxX);
      for (int x = minX; x <= maxX && x < _w; x++) _setPixel(x, y, g);
    }
    _dirty = true;
  }

  void drawLine(int x0, int y0, int x1, int y1, uint16_t color) {
    uint8_t g = rgb565ToGray4(color);
    int dx = abs(x1-x0), dy = abs(y1-y0);
    int sx = x0<x1 ? 1 : -1, sy = y0<y1 ? 1 : -1;
    int err = dx-dy;
    while (true) {
      _setPixel(x0, y0, g);
      if (x0==x1 && y0==y1) break;
      int e2 = 2*err;
      if (e2 > -dy) { err -= dy; x0 += sx; }
      if (e2 < dx) { err += dx; y0 += sy; }
    }
    _dirty = true;
  }

  void setTextColor(uint16_t fg, uint16_t bg) {
    _fg = rgb565ToGray4(fg);
    _bg = rgb565ToGray4(bg);
  }
  void setTextColor(uint16_t fg) { _fg = rgb565ToGray4(fg); }

  void setTextSize(uint8_t s) { _textSize = s; }

  void setTextDatum(uint8_t d) { _datum = d; }

  void setCursor(int x, int y) { _curX = x; _curY = y; }

  // Basic 6×8 font rendering scaled by _textSize
  void print(char c) {
    if (c < 0x20 || c > 0x7E) c = '?';
    _drawChar(c, _curX, _curY, _fg, _bg, _textSize);
    _curX += 6 * _textSize;
    _dirty = true;
  }

  void print(const char* s) {
    while (*s) { print(*s); s++; }
  }

  void printf(const char* fmt, ...) {
    char buf[128];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    print(buf);
  }

  void drawString(const char* s, int x, int y) {
    int len = strlen(s);
    int w = len * 6 * _textSize;
    int h = 8 * _textSize;
    if (_datum == 1) { // MC_DATUM
      x -= w / 2;
      y -= h / 2;
    }
    setCursor(x, y);
    print(s);
  }

  void pushSprite(int x, int y) {
    if (!fb || !_dirty) return;
    uint32_t now = millis();
    bool fullRefresh = (now - _lastFullRefresh > FULL_REFRESH_INTERVAL) || _lastFullRefresh == 0;

    // Copy our 4bpp packed fb into epdiy's high-level 8bpp framebuffer
    uint8_t* hlFb = epd_hl_get_framebuffer(&_hl);
    int stride4 = _w / 2;
    for (int py = 0; py < _h; py++) {
      for (int px = 0; px < _w; px++) {
        int idx4 = (py * _w + px) / 2;
        uint8_t gray4;
        if (px & 1) gray4 = fb[idx4] & 0x0F;
        else        gray4 = (fb[idx4] >> 4) & 0x0F;
        // epdiy 8bpp: upper nibble is the value
        hlFb[py * _w + px] = gray4 << 4;
      }
    }

    if (fullRefresh) {
      epd_poweron();
      epd_clear();
      epd_hl_update_screen(&_hl, MODE_GC16, 25);
      epd_poweroff();
      _lastFullRefresh = now;
    } else {
      epd_poweron();
      epd_hl_update_screen(&_hl, MODE_DU, 25);
      epd_poweroff();
    }
    memcpy(prevFb, fb, (_w * _h) / 2);
    _dirty = false;
  }

  int width() { return _w; }
  int height() { return _h; }

  void drawPixel(int x, int y, uint16_t color) {
    _setPixel(x, y, rgb565ToGray4(color));
    _dirty = true;
  }

  void setBrightness(int) {} // EPD has no backlight

private:
  void _setPixel(int x, int y, uint8_t gray4) {
    if (x < 0 || x >= _w || y < 0 || y >= _h) return;
    int idx = (y * _w + x) / 2;
    if (x & 1) {
      fb[idx] = (fb[idx] & 0xF0) | (gray4 & 0x0F);
    } else {
      fb[idx] = (fb[idx] & 0x0F) | ((gray4 & 0x0F) << 4);
    }
  }

  void _triEdge(int x0, int y0, int x1, int y1, int y, int& minX, int& maxX) {
    if ((y0 <= y && y1 >= y) || (y1 <= y && y0 >= y)) {
      if (y0 == y1) { minX = min(minX, min(x0,x1)); maxX = max(maxX, max(x0,x1)); return; }
      int x = x0 + (int)((long)(y - y0) * (x1 - x0) / (y1 - y0));
      minX = min(minX, x);
      maxX = max(maxX, x);
    }
  }

  // Minimal 6×8 bitmap font renderer
  void _drawChar(char c, int x, int y, uint8_t fg, uint8_t bg, uint8_t sz);
};

// Color constants
#ifndef GREEN
#define GREEN 0x07E0
#endif
#ifndef RED
#define RED 0xF800
#endif
#ifndef MC_DATUM
#define MC_DATUM 1
#endif
#ifndef TL_DATUM
#define TL_DATUM 0
#endif

using TFT_eSPI = EpdSprite;
using TFT_eSprite = EpdSprite;

// Stub types
struct RTC_TimeTypeDef { uint8_t Hours, Minutes, Seconds; RTC_TimeTypeDef() : Hours(0), Minutes(0), Seconds(0) {} };
struct RTC_DateTypeDef { uint8_t WeekDay, Month, Date; uint16_t Year; RTC_DateTypeDef() : WeekDay(0), Month(1), Date(1), Year(2026) {} };

struct HalImu {
  void Init() {}
  void getAccelData(float* ax, float* ay, float* az) {
    *ax = 0; *ay = -1.0f; *az = 0;
  }
};

struct HalAxp {
  void ScreenBreath(int) {}
  void SetLDO2(bool) {}
  void PowerOff() { esp_deep_sleep_start(); }
  float GetBatVoltage() { return 4.2f; }
  float GetBatCurrent() { return 0; }
  float GetVBusVoltage() { return 5.0f; }
  float GetTempInAXP192() { return 25.0f; }
  uint8_t GetBtnPress() { return 0; }
};

struct HalRtc {
  RTC_TimeTypeDef _t;
  RTC_DateTypeDef _d;
  void SetTime(RTC_TimeTypeDef* t) { _t = *t; }
  void SetDate(RTC_DateTypeDef* d) { _d = *d; }
  void GetTime(RTC_TimeTypeDef* t) { *t = _t; }
  void GetDate(RTC_DateTypeDef* d) { *d = _d; }
};

struct HalBeep {
  void begin() {}
  void update() {}
  void tone(uint16_t, uint16_t) {}
};

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

struct HalEpd47 {
  void init() {
    epd_init(EPD_OPTIONS_DEFAULT);
    epd_poweron();
    epd_clear();
    epd_poweroff();
  }
  void setRotation(int) {}
  void setBrightness(int) {}
  void fillScreen(uint16_t) {}
  void setTextDatum(uint8_t) {}
  void setTextSize(uint8_t) {}
  void setTextColor(uint16_t, uint16_t) {}
  void drawString(const char*, int, int) {}
};

extern HalEpd47 tft;

struct HalM5 {
  HalImu Imu;
  HalAxp Axp;
  HalRtc Rtc;
  HalBeep Beep;
  HalButton BtnA;
  HalButton BtnB;
  HalButton BtnC; // third button for menu
  HalEpd47& Lcd;

  EpdSprite* _spr = nullptr;

  HalM5(HalEpd47& lcd) : Lcd(lcd) {}

  void setSpr(EpdSprite* s) { _spr = s; }

  void begin() {
    Lcd.init();
    if (_spr) _spr->_hl = epd_hl_init(EPD_BUILTIN_WAVEFORM);
    pinMode(EPD_BTN1, INPUT_PULLUP);
    pinMode(EPD_BTN2, INPUT_PULLUP);
    pinMode(EPD_BTN3, INPUT_PULLUP);
  }

  void update() {
    BtnA.update(!digitalRead(EPD_BTN1));
    BtnB.update(!digitalRead(EPD_BTN2));
    BtnC.update(!digitalRead(EPD_BTN3));
  }
};

extern HalM5 M5;

#endif // BOARD_LILYGO_EPD47
