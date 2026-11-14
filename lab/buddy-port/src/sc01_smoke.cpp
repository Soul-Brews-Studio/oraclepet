// ═══════════════════════════════════════════════════════════════════════
//  sc01_smoke.cpp — standalone bring-up test for the WT32-SC01 Plus display.
//
//  Verifies the four unknowns in lgfx_sc01plus.h on the real panel:
//    1. freq_write   — screen stable (no tearing/garbage)?
//    2. invert       — backgrounds the right polarity (not a photo-negative)?
//    3. rgb_order    — the R/G/B bars read Red, Green, Blue (not B/G/R)?
//    4. touch        — tap coords printed to serial match where you touched?
//
//  Build (does NOT pull in main.cpp / the M5 buddy code):
//     pio run -e wt32-sc01-plus-smoke -t upload
//     # or via the root Makefile? no — that's ESPHome. Use pio here:
//     cd lab/buddy-port && pio run -e wt32-sc01-plus-smoke -t upload -t monitor
// ═══════════════════════════════════════════════════════════════════════
#include <Arduino.h>
#include "lgfx_sc01plus.h"

static LGFX_SC01Plus lcd;

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[sc01-smoke] init…");

  lcd.init();
  lcd.setBrightness(200);
  lcd.setRotation(0);                 // 0 = 320×480 portrait

  // (2) invert + (3) rgb_order check — three pure bars + labels.
  lcd.fillScreen(TFT_BLACK);
  const int w = lcd.width(), third = lcd.height() / 3;
  lcd.fillRect(0, 0,         w, third, TFT_RED);
  lcd.fillRect(0, third,     w, third, TFT_GREEN);
  lcd.fillRect(0, third * 2, w, third, TFT_BLUE);

  lcd.setTextColor(TFT_WHITE, TFT_BLACK);
  lcd.setTextDatum(textdatum_t::middle_center);
  lcd.setTextSize(2);
  lcd.drawString("RED   (top)",    w / 2, third / 2);
  lcd.drawString("GREEN (mid)",    w / 2, third + third / 2);
  lcd.drawString("BLUE  (bottom)", w / 2, third * 2 + third / 2);

  Serial.printf("[sc01-smoke] %dx%d up. Bars should read R/G/B top→bottom.\n",
                lcd.width(), lcd.height());
  Serial.println("[sc01-smoke] If negative image -> flip cfg.invert.");
  Serial.println("[sc01-smoke] If bars are B/G/R   -> flip cfg.rgb_order.");
  Serial.println("[sc01-smoke] Tap the screen; coords print below.");
}

void loop() {
  // (4) touch check — print coords, dot where you touch.
  int32_t x, y;
  if (lcd.getTouch(&x, &y)) {
    lcd.fillCircle(x, y, 4, TFT_WHITE);
    Serial.printf("[sc01-smoke] touch  x=%ld  y=%ld\n", (long)x, (long)y);
    delay(40);
  }
}
