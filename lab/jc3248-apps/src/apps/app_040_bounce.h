#pragma once
// 040 bounce — colored balls bouncing off the walls, each with a fading trail.
// LOOK FOR: 5 different-colored balls drifting around the 320x480 screen, bouncing
// off all four edges, each dragging a short comet-tail of dimmer circles behind it.
#if APP == 40
static LGFX_Sprite spr;

#define BOUNCE_BALLS 5      // number of balls
#define BOUNCE_TRAIL 8      // trail history length per ball

struct BounceBall {
  float x, y;     // current center
  float vx, vy;   // velocity (px/frame)
  int   r;        // radius
  uint8_t cr, cg, cb;  // base color (8-bit each)
  float tx[BOUNCE_TRAIL];  // trail history x
  float ty[BOUNCE_TRAIL];  // trail history y
};

static BounceBall balls[BOUNCE_BALLS];
static bool bounce_inited = false;

static void app_setup() {
  Serial.println("[app040] bounce");
  spr.setColorDepth(16);
  spr.setPsram(true);
  spr.createSprite(axs::W, axs::H);

  // distinct base colors for each ball
  static const uint8_t pal[BOUNCE_BALLS][3] = {
    { 255,  60,  60 },   // red
    {  60, 200, 255 },   // cyan
    { 255, 220,  40 },   // yellow
    { 120, 255, 120 },   // green
    { 230, 110, 255 },   // magenta
  };

  for (int i = 0; i < BOUNCE_BALLS; i++) {
    BounceBall &b = balls[i];
    b.r  = 12 + (i % 3) * 4;                 // radii 12..20
    b.x  = (float)random(b.r, axs::W - b.r);
    b.y  = (float)random(b.r, axs::H - b.r);
    // random nonzero velocities, magnitude ~1.5..4 px/frame
    float ang = (float)random(0, 628) * 0.01f;   // 0..2pi
    float spd = 1.8f + (float)random(0, 220) * 0.01f;
    b.vx = cosf(ang) * spd;
    b.vy = sinf(ang) * spd;
    if (fabsf(b.vx) < 0.5f) b.vx = (b.vx < 0 ? -0.8f : 0.8f);
    if (fabsf(b.vy) < 0.5f) b.vy = (b.vy < 0 ? -0.8f : 0.8f);
    b.cr = pal[i][0];
    b.cg = pal[i][1];
    b.cb = pal[i][2];
    for (int t = 0; t < BOUNCE_TRAIL; t++) { b.tx[t] = b.x; b.ty[t] = b.y; }
  }
  bounce_inited = true;
}

static void app_loop() {
  spr.fillSprite(0x0000);  // clear (cheap)

  for (int i = 0; i < BOUNCE_BALLS; i++) {
    BounceBall &b = balls[i];

    // shift trail history (oldest drops off), record current position at head
    for (int t = BOUNCE_TRAIL - 1; t > 0; t--) {
      b.tx[t] = b.tx[t - 1];
      b.ty[t] = b.ty[t - 1];
    }
    b.tx[0] = b.x;
    b.ty[0] = b.y;

    // integrate position
    b.x += b.vx;
    b.y += b.vy;

    // reflect off the 4 walls, clamp inside so it never sticks
    if (b.x < b.r)            { b.x = b.r;            b.vx = -b.vx; }
    if (b.x > axs::W - b.r)   { b.x = axs::W - b.r;   b.vx = -b.vx; }
    if (b.y < b.r)            { b.y = b.r;            b.vy = -b.vy; }
    if (b.y > axs::H - b.r)   { b.y = axs::H - b.r;   b.vy = -b.vy; }

    // draw the trail: older = dimmer + slightly smaller (drawn first, behind ball)
    for (int t = BOUNCE_TRAIL - 1; t >= 0; t--) {
      // fade factor: head (t=0) brightest, tail dimmest
      float f = 1.0f - (float)(t + 1) / (float)(BOUNCE_TRAIL + 1);  // ~0.1..0.9
      uint8_t r = (uint8_t)(b.cr * f);
      uint8_t g = (uint8_t)(b.cg * f);
      uint8_t bl = (uint8_t)(b.cb * f);
      int tr = b.r - (t * b.r) / (BOUNCE_TRAIL + 2);
      if (tr < 2) tr = 2;
      spr.fillCircle((int)b.tx[t], (int)b.ty[t], tr, spr.color565(r, g, bl));
    }

    // draw the ball itself (full brightness) with a bright highlight dot
    spr.fillCircle((int)b.x, (int)b.y, b.r, spr.color565(b.cr, b.cg, b.cb));
    // small lighter highlight toward upper-left for a glossy look
    uint8_t hr = (uint8_t)(b.cr + (255 - b.cr) * 0.6f);
    uint8_t hg = (uint8_t)(b.cg + (255 - b.cg) * 0.6f);
    uint8_t hb = (uint8_t)(b.cb + (255 - b.cb) * 0.6f);
    spr.fillCircle((int)b.x - b.r / 3, (int)b.y - b.r / 3, b.r / 3,
                   spr.color565(hr, hg, hb));
  }

  axs::push((const uint16_t*)spr.getBuffer());
  delay(16);  // ~loop pacing
}
#endif
