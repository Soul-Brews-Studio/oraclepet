#pragma once
// 044 sine_waves — three overlaid scrolling sine waves (cyan/magenta/yellow).
// LOOK FOR: three smooth horizontal waves of different colors gliding across a black screen at different speeds, with a soft fill glowing under the cyan wave.
#if APP == 44
static LGFX_Sprite spr;

// Per-wave parameters: color, amplitude (px), wavelength (px), phase speed (rad/ms), vertical center offset.
struct Wave {
  uint16_t color;
  float    amp;
  float    waveLen;   // pixels per full cycle
  float    speed;     // radians per millisecond (phase scroll rate)
  float    yc;        // vertical center (px)
};

static Wave waves[3];

// Lookup of x->y for the cyan wave so we can fill underneath cheaply.
static int cyanY[axs::W];

static void app_setup() {
  Serial.println("[app044] sine_waves");
  spr.setColorDepth(16);
  spr.setPsram(true);
  spr.createSprite(axs::W, axs::H);

  // Cyan: broad, slow, lowest. Magenta: medium. Yellow: tight, fast, highest.
  waves[0] = { spr.color565(0,   255, 255), 70.0f, 220.0f, 0.0016f, axs::H * 0.62f }; // cyan
  waves[1] = { spr.color565(255, 0,   255), 55.0f, 150.0f, 0.0026f, axs::H * 0.50f }; // magenta
  waves[2] = { spr.color565(255, 255, 0  ), 40.0f, 95.0f,  0.0040f, axs::H * 0.38f }; // yellow
}

static inline int waveY(const Wave& w, int x, float t) {
  // phase = position term + time scroll term
  float ph = (float)x * (2.0f * 3.14159265f / w.waveLen) + t * w.speed;
  return (int)(w.yc + w.amp * sinf(ph));
}

static void app_loop() {
  spr.fillSprite(0x0000);

  float t = (float)millis();

  // --- Soft fill under the cyan wave (drawn first, behind the lines) ---
  // Compute cyan y for every column, then paint vertical fades downward.
  const Wave& cw = waves[0];
  uint16_t fillCol = spr.color565(0, 40, 55); // dim cyan glow
  for (int x = 0; x < axs::W; x++) {
    int y = waveY(cw, x, t);
    if (y < 0) y = 0;
    if (y >= axs::H) y = axs::H - 1;
    cyanY[x] = y;
    spr.drawFastVLine(x, y, axs::H - y, fillCol);
  }

  // --- Draw the three waves as connected line segments ---
  for (int wi = 0; wi < 3; wi++) {
    const Wave& w = waves[wi];
    int prevX = 0;
    int prevY = waveY(w, 0, t);
    // Step in 4px chunks for speed; lines connect the samples smoothly.
    for (int x = 4; x <= axs::W; x += 4) {
      int xx = (x > axs::W - 1) ? axs::W - 1 : x;
      int y  = waveY(w, xx, t);
      // Draw a 2px-thick stroke for a bolder look.
      spr.drawLine(prevX, prevY,     xx, y,     w.color);
      spr.drawLine(prevX, prevY + 1, xx, y + 1, w.color);
      prevX = xx;
      prevY = y;
    }
  }

  axs::push((const uint16_t*)spr.getBuffer());
  delay(16);
}
#endif
