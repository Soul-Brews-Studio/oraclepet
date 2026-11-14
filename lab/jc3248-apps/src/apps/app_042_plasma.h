#pragma once
// 042 plasma — classic sine-field plasma through a moving rainbow palette.
// LOOK FOR: a full-screen flowing field of smooth rainbow blobs that drift, blend, and pulse forever.
#if APP == 42
static LGFX_Sprite spr;

// Coarse grid: 16px blocks -> 20 cols x 30 rows over 320x480.
static constexpr int BS   = 16;            // block size in pixels
static constexpr int COLS = axs::W / BS;   // 20
static constexpr int ROWS = axs::H / BS;   // 30

// Precomputed per-axis sine contributions (depend only on x or y, plus t which we add live).
// We store the spatial phase so we only call sinf() per cell, not per pixel.
static float colPhaseX[COLS];   // x*k1
static float colPhaseXY[COLS];  // x*k3 part of (x+y)
static float rowPhaseY[ROWS];   // y*k2
static float rowPhaseXY[ROWS];  // y*k3 part of (x+y)

// 256-entry rainbow palette (rgb565). Smooth HSV-style sweep around the hue wheel.
static uint16_t palette[256];

static void app_setup() {
  Serial.println("[app042] plasma");
  spr.setColorDepth(16);
  spr.setPsram(true);
  spr.createSprite(axs::W, axs::H);

  // Spatial frequencies (radians per pixel). Different k's keep the field non-repeating.
  const float k1 = 0.045f;   // x term
  const float k2 = 0.060f;   // y term
  const float k3 = 0.038f;   // (x+y) diagonal term

  for (int cx = 0; cx < COLS; ++cx) {
    int px = cx * BS + BS / 2;          // pixel center of the cell
    colPhaseX[cx]  = px * k1;
    colPhaseXY[cx] = px * k3;
  }
  for (int cy = 0; cy < ROWS; ++cy) {
    int py = cy * BS + BS / 2;
    rowPhaseY[cy]  = py * k2;
    rowPhaseXY[cy] = py * k3;
  }

  // Build a continuous rainbow palette. Hue 0..1 around the wheel, full sat/value.
  for (int i = 0; i < 256; ++i) {
    float h = i / 256.0f * 6.0f;        // 0..6 sectors
    int   s = (int)h;                   // sector index
    float f = h - s;                    // fractional within sector
    uint8_t up   = (uint8_t)(f * 255.0f);
    uint8_t down = (uint8_t)((1.0f - f) * 255.0f);
    uint8_t r = 0, g = 0, b = 0;
    switch (s % 6) {
      case 0: r = 255;  g = up;   b = 0;    break;
      case 1: r = down; g = 255;  b = 0;    break;
      case 2: r = 0;    g = 255;  b = up;   break;
      case 3: r = 0;    g = down; b = 255;  break;
      case 4: r = up;   g = 0;    b = 255;  break;
      case 5: r = 255;  g = 0;    b = down; break;
    }
    palette[i] = spr.color565(r, g, b);
  }
}

static void app_loop() {
  // Time drives both the wave phases and a slow palette rotation.
  float ms = (float)millis();
  float t  = ms * 0.0018f;        // wave animation speed
  uint8_t palShift = (uint8_t)((ms * 0.04f));  // moving palette offset

  // Precompute the t-shifted column contributions once per frame (COLS sinf calls).
  static float cX[COLS];
  static float cXY[COLS];
  for (int cx = 0; cx < COLS; ++cx) {
    cX[cx]  = sinf(colPhaseX[cx]  + t);
    cXY[cx] = colPhaseXY[cx];      // diagonal phase combined with row below
  }

  for (int cy = 0; cy < ROWS; ++cy) {
    float sY    = sinf(rowPhaseY[cy] + t);
    float yDiag = rowPhaseXY[cy];
    int   py    = cy * BS;
    for (int cx = 0; cx < COLS; ++cx) {
      // value = sin(x*k1+t) + sin(y*k2+t) + sin((x+y)*k3 + t)
      float v = cX[cx] + sY + sinf(cXY[cx] + yDiag + t);
      // v is in [-3,3] -> map to [0,255]
      uint8_t idx = (uint8_t)((v + 3.0f) * (255.0f / 6.0f));
      uint16_t color = palette[(uint8_t)(idx + palShift)];
      spr.fillRect(cx * BS, py, BS, BS, color);
    }
  }

  axs::push((const uint16_t*)spr.getBuffer());
  delay(16);
}
#endif
