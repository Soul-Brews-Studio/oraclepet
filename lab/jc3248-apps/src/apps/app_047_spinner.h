#pragma once
// 047 spinner — classic rotating loading spinner: ring of dots whose brightness
//                chases around the center, plus a breathing outer circle.
// LOOK FOR: a centered ring of 12 dots with a bright "comet head" rotating around
//           it, surrounded by a thin outer circle that pulses/breathes in and out.
//           Colors drift slowly through the hue wheel on a dark background.
#if APP == 47
static LGFX_Sprite spr;

// Simple hue->rgb565 helper (h in [0,1)). Lightweight, called a handful of times/frame.
static uint16_t spin_hue(float h, float v) {
  h -= floorf(h);
  float r = 0, g = 0, b = 0;
  float hh = h * 6.0f;
  int   i  = (int)hh;
  float f  = hh - i;
  float p  = 0.0f;
  float q  = 1.0f - f;
  float t  = f;
  switch (i % 6) {
    case 0: r = 1; g = t; b = p; break;
    case 1: r = q; g = 1; b = p; break;
    case 2: r = p; g = 1; b = t; break;
    case 3: r = p; g = q; b = 1; break;
    case 4: r = t; g = p; b = 1; break;
    default:r = 1; g = p; b = q; break;
  }
  uint8_t R = (uint8_t)(r * v * 255.0f);
  uint8_t G = (uint8_t)(g * v * 255.0f);
  uint8_t B = (uint8_t)(b * v * 255.0f);
  return spr.color565(R, G, B);
}

static void app_setup() {
  Serial.println("[app047] spinner");
  spr.setColorDepth(16);
  spr.setPsram(true);
  spr.createSprite(axs::W, axs::H);
}

static void app_loop() {
  spr.fillSprite(0x0000);             // clear (cheap)

  const int   cx       = axs::W / 2;
  const int   cy       = axs::H / 2;
  const int   N        = 12;          // dots in the ring
  const float ringR    = 70.0f;       // ring radius
  const float TWO_PI_F = 6.2831853f;

  float t   = millis() * 0.001f;      // seconds
  float hue = t * 0.07f;              // slow color drift

  // --- Breathing outer circle ---
  // radius oscillates around a base; thin double ring for a soft glow look.
  float breathe = sinf(t * 1.6f) * 0.5f + 0.5f;   // 0..1
  float outR    = 100.0f + breathe * 26.0f;
  uint16_t outCol = spin_hue(hue + 0.5f, 0.35f + breathe * 0.55f);
  spr.drawCircle(cx, cy, (int)outR, outCol);
  spr.drawCircle(cx, cy, (int)outR - 1, outCol);

  // --- Rotating ring of dots ---
  // The "head" position sweeps the ring; each dot's brightness depends on its
  // angular distance behind the head -> classic chasing/fading spinner.
  float head = t * 1.4f;              // revolutions over time (in turns)
  head -= floorf(head);              // 0..1
  for (int k = 0; k < N; ++k) {
    float frac = (float)k / (float)N;          // 0..1 around the ring
    float ang  = frac * TWO_PI_F - 1.5707963f; // start at top
    int   dx   = cx + (int)(cosf(ang) * ringR);
    int   dy   = cy + (int)(sinf(ang) * ringR);

    // distance "behind" the head, going around the ring (0 = at head)
    float d = frac - head;
    d -= floorf(d);                  // 0..1, wraps

    // brightness: bright near the head, fading as we trail behind
    float bright = 1.0f - d;         // 1 at head -> 0 just behind
    bright = bright * bright;        // sharpen the comet head
    float v = 0.18f + bright * 0.82f;

    int      rad = 6 + (int)(bright * 5.0f);   // head dot is bigger
    uint16_t col = spin_hue(hue + frac * 0.12f, v);
    spr.fillCircle(dx, dy, rad, col);
  }

  // --- Center pip (steady glow) ---
  float pip = sinf(t * 3.0f) * 0.5f + 0.5f;
  spr.fillCircle(cx, cy, 5 + (int)(pip * 3.0f), spin_hue(hue + 0.25f, 0.6f + pip * 0.4f));

  axs::push((const uint16_t*)spr.getBuffer());   // push the frame to the panel
  delay(16);                          // ~60fps target (panel caps lower)
}
#endif
