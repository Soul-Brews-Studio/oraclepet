#pragma once
// ─────────────────────────────────────────────────────────────────────────
//  ws_7inch_rgb — display HAL for the Waveshare ESP32-S3-Touch-LCD-7 (800×480 RGB).
//  REWRITTEN to mirror the PROVEN somtor-meter structure EXACTLY:
//    - bus / rgbDisplay / canvas are GLOBAL STATIC objects (constructed at file
//      scope), NOT `new`-ed inside init(). new-in-init left the panel BLANK.
//    - only canvas.begin() is called (it begins the RGB display itself).
//    - canvas.setRotation(1) → 480×800 portrait (somtor does this).
//  Exposes the same `namespace axs` API so main.cpp is unchanged.
//
//  Pins verified from somtor main.cpp:21-29 (env esp32s3-lcd7). CH422G sequence +
//  GT911 reset/probe identical to somtor ch422g.h.
// ─────────────────────────────────────────────────────────────────────────
#if defined(BOARD_WAVESHARE_7INCH)

#include <Arduino.h>
#include <Wire.h>
#include <driver/gpio.h>
#include <Arduino_GFX_Library.h>

namespace axs {

// ── Geometry ──────────────────────────────────────────────────────────────
static constexpr int PANEL_W = 800, PANEL_H = 480;   // native (Canvas ctor dims)
static constexpr int VIEW_W  = 480, VIEW_H  = 800;   // AFTER setRotation(1) — portrait
static constexpr int PET_W   = 320, PET_H   = 480;   // pet sprite — MUST match pet.cpp
static constexpr int OFF_X   = (VIEW_W - PET_W) / 2; // 80  — center pet on portrait canvas
static constexpr int OFF_Y   = (VIEW_H - PET_H) / 2; // 160

// ── I²C / expander / touch ─────────────────────────────────────────────────
static constexpr int     I2C_SDA = 8, I2C_SCL = 9;
static constexpr uint8_t CH422G_MODE_ADDR = 0x24;
static constexpr uint8_t CH422G_OUT_ADDR  = 0x38;
static constexpr gpio_num_t GT911_INT = GPIO_NUM_4;

// ── GLOBAL STATIC display objects — the somtor pattern (NOT new-in-init) ────
static Arduino_ESP32RGBPanel s_panel(
  /* DE, VSYNC, HSYNC, PCLK */ 5, 3, 46, 7,
  /* R0..R4 */ 1, 2, 42, 41, 40,
  /* G0..G5 */ 39, 0, 45, 48, 47, 21,
  /* B0..B4 */ 14, 38, 18, 17, 10,
  /* hsync polarity, front, pulse, back */ 0, 8, 4, 8,
  /* vsync polarity, front, pulse, back */ 0, 8, 4, 8,
  /* pclk_active_neg, prefer_speed       */ 1, 16000000);
static Arduino_RGB_Display s_rgb(PANEL_W, PANEL_H, &s_panel, 0 /*rot*/, true /*auto_flush*/);
static Arduino_Canvas      s_canvas(PANEL_W, PANEL_H, &s_rgb);
static Arduino_GFX* const  s_gfx = &s_canvas;   // draw target (rotated to 480×800)
static uint8_t             s_gt911 = 0;

// ── CH422G output write ───────────────────────────────────────────────────
static void ch422gOut(uint8_t v) {
  Wire.beginTransmission(CH422G_OUT_ADDR);
  Wire.write(v);
  Wire.endTransmission();
}

// ── GT911 I²C helpers ─────────────────────────────────────────────────────
static bool gt911Read(uint8_t addr, uint16_t reg, uint8_t* buf, size_t len) {
  Wire.beginTransmission(addr);
  Wire.write((uint8_t)(reg >> 8)); Wire.write((uint8_t)(reg & 0xFF));
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)addr, (int)len) != (int)len) return false;
  for (size_t i = 0; i < len; i++) buf[i] = Wire.read();
  return true;
}
static bool gt911Write(uint8_t addr, uint16_t reg, uint8_t val) {
  Wire.beginTransmission(addr);
  Wire.write((uint8_t)(reg >> 8)); Wire.write((uint8_t)(reg & 0xFF)); Wire.write(val);
  return Wire.endTransmission() == 0;
}
static void gt911Probe() {
  uint8_t id[4];
  if (gt911Read(0x5D, 0x8140, id, 4)) { s_gt911 = 0x5D; return; }
  if (gt911Read(0x14, 0x8140, id, 4)) { s_gt911 = 0x14; return; }
  s_gt911 = 0;
}

// ── init(): CH422G power/reset (= somtor ch422gInit), then canvas.begin() ──
static bool init(bool warmup = true) {
  (void)warmup;

  // CH422G sequence — byte-for-byte the somtor ch422gInit(8,9).
  Wire.begin(I2C_SDA, I2C_SCL, 400000);
  delay(10);
  Wire.beginTransmission(CH422G_MODE_ADDR); Wire.write(0x01); Wire.endTransmission();
  delay(10);
  ch422gOut(0x14);
  delay(10);
  gpio_reset_pin(GT911_INT);
  gpio_set_direction(GT911_INT, GPIO_MODE_OUTPUT);
  gpio_set_level(GT911_INT, 0);
  delay(2);
  ch422gOut(0x1E);
  delay(10); delay(100);
  gpio_set_direction(GT911_INT, GPIO_MODE_INPUT);
  delay(50);

  // ONLY canvas.begin() — it begins the underlying RGB display (somtor pattern).
  if (!s_canvas.begin()) { Serial.println("[ws7] canvas.begin() FAILED (PSRAM?)"); return false; }
  s_canvas.setRotation(1);                 // 800×480 → 480×800 portrait (somtor)
  s_canvas.fillScreen(0x0000);
  s_canvas.flush();

  gt911Probe();
  Serial.printf("[ws7] init OK. gt911 @ 0x%02X, view %dx%d, pet @ (%d,%d)\n",
                s_gt911, s_canvas.width(), s_canvas.height(), OFF_X, OFF_Y);
  return true;
}

// ── fill_solid(): whole-canvas solid colour ────────────────────────────────
static void fill_solid(uint16_t color) {
  s_gfx->fillScreen(color);
  s_gfx->flush();
}

// ── push(): center the 320×480 pet sprite (RGB565) on the 480×800 canvas ───
// ⚠️ VERIFY byte order on hardware (LGFX swap565 vs Arduino_GFX RGB565).
static void push(const uint16_t* buf) {
  if (!buf) return;
  s_gfx->fillScreen(0x0000);                                   // letterbox bars
  s_gfx->draw16bitRGBBitmap(OFF_X, OFF_Y, (uint16_t*)buf, PET_W, PET_H);
  s_gfx->flush();
}
static void push_region(int, int, int, int, const uint16_t* buf) { push(buf); }

// ── set_backlight(): CH422G enable bit (on/off) ────────────────────────────
static void set_backlight(uint8_t level) { ch422gOut(level ? 0x1E : 0x14); }

// ── read_touch(): GT911 → pet sprite frame ─────────────────────────────────
static bool read_touch(int16_t* x, int16_t* y) {
  if (s_gt911 == 0) { gt911Probe(); if (s_gt911 == 0) return false; }
  uint8_t status;
  if (!gt911Read(s_gt911, 0x814E, &status, 1)) return false;
  if (!(status & 0x80) || (status & 0x0F) == 0) { gt911Write(s_gt911, 0x814E, 0); return false; }
  uint8_t pt[6];
  bool ok = gt911Read(s_gt911, 0x8150, pt, 6);
  gt911Write(s_gt911, 0x814E, 0);
  if (!ok) return false;
  int rx = pt[0] | (pt[1] << 8);
  int ry = pt[2] | (pt[3] << 8);
  if (rx < 0 || rx >= VIEW_W || ry < 0 || ry >= VIEW_H) return false;
  rx -= OFF_X; ry -= OFF_Y;
  if (rx < 0 || rx >= PET_W || ry < 0 || ry >= PET_H) return false;
  *x = (int16_t)rx; *y = (int16_t)ry;
  return true;
}

} // namespace axs

#endif // BOARD_WAVESHARE_7INCH
