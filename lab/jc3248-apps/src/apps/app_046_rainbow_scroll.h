#pragma once
// 046 rainbow_scroll — smooth vertical rainbow gradient scrolling upward forever.
// LOOK FOR: full-width horizontal color bands cycling through the rainbow, the whole
//           gradient sliding steadily upward, saturated and bright, never repeating dully.
#if APP == 46
static LGFX_Sprite spr;

// HSV (h in 0..360, s/v in 0..1) -> rgb565. Saturated, bright bands.
static uint16_t hsv565(float h, float s, float v) {
  h = fmodf(h, 360.0f);
  if (h < 0) h += 360.0f;
  float c = v * s;
  float hp = h / 60.0f;
  float x = c * (1.0f - fabsf(fmodf(hp, 2.0f) - 1.0f));
  float r = 0, g = 0, b = 0;
  if      (hp < 1) { r = c; g = x; b = 0; }
  else if (hp < 2) { r = x; g = c; b = 0; }
  else if (hp < 3) { r = 0; g = c; b = x; }
  else if (hp < 4) { r = 0; g = x; b = c; }
  else if (hp < 5) { r = x; g = 0; b = c; }
  else             { r = c; g = 0; b = x; }
  float m = v - c;
  uint8_t R = (uint8_t)((r + m) * 255.0f);
  uint8_t G = (uint8_t)((g + m) * 255.0f);
  uint8_t B = (uint8_t)((b + m) * 255.0f);
  return (uint16_t)(((R & 0xF8) << 8) | ((G & 0xFC) << 3) | (B >> 3));
}

static const int BANDS = 36;                 // ~36 horizontal bands across 480px height
static const int BAND_H = (axs::H + BANDS - 1) / BANDS;  // band height (rounds up to cover)

static void app_setup() {
  Serial.println("[app046] rainbow_scroll");
  spr.setColorDepth(16);
  spr.setPsram(true);
  spr.createSprite(axs::W, axs::H);
}

static void app_loop() {
  static uint32_t frame = 0;

  // Scroll upward: top of screen advances through hue space over time.
  // A full hue sweep (0..360) maps across the BANDS, and 'frame' shifts the phase.
  float scroll = (float)frame * 2.2f;        // upward scroll speed in hue-degrees/frame
  float slowShift = sinf((float)frame * 0.01f) * 12.0f; // gentle global drift for life

  for (int i = 0; i < BANDS; ++i) {
    // Hue increases down the screen; subtract scroll so the pattern moves UP over time.
    float hue = ((float)i / (float)BANDS) * 360.0f - scroll + slowShift;
    // Subtle brightness shimmer so bands feel alive without losing saturation.
    float v = 0.92f + 0.08f * sinf((float)i * 0.5f + (float)frame * 0.05f);
    uint16_t col = hsv565(hue, 1.0f, v);
    int y = i * BAND_H;
    int h = BAND_H;
    if (y + h > axs::H) h = axs::H - y;
    if (h > 0) spr.fillRect(0, y, axs::W, h, col);
  }

  axs::push((const uint16_t*)spr.getBuffer());
  frame++;
  delay(16);
}
#endif
