#pragma once
// 045 fireworks — particle fireworks: rockets rise, burst, spread, fade.
// LOOK FOR: glowing rockets launching from the bottom, exploding into colored sparks that arc down with gravity and fade to black.
#if APP == 45
static LGFX_Sprite spr;

// ---- particle system ----------------------------------------------------
struct Particle {
  float x, y;     // position
  float vx, vy;   // velocity (pixels/frame)
  float life;     // 1.0 -> 0.0
  float decay;    // life lost per frame
  uint8_t r, g, b;// base color (8-bit)
  bool active;
};

static const int   MAX_PARTICLES = 200;
static Particle    parts[MAX_PARTICLES];

// ---- rockets ------------------------------------------------------------
struct Rocket {
  float x, y;     // position
  float vy;       // upward velocity (negative)
  float targetY;  // explosion altitude
  uint8_t r, g, b;// burst color
  bool active;
};

static const int MAX_ROCKETS = 4;
static Rocket    rockets[MAX_ROCKETS];

static const float GRAVITY = 0.10f;

// pick a vivid color by hue-ish random palette
static void rndColor(uint8_t &r, uint8_t &g, uint8_t &b) {
  switch (random(6)) {
    case 0: r = 255; g = 80;  b = 80;  break; // red
    case 1: r = 255; g = 200; b = 60;  break; // gold
    case 2: r = 80;  g = 255; b = 120; break; // green
    case 3: r = 90;  g = 160; b = 255; break; // blue
    case 4: r = 255; g = 120; b = 255; break; // magenta
    default:r = 120; g = 255; b = 255; break; // cyan
  }
}

static int findFreeParticle() {
  for (int i = 0; i < MAX_PARTICLES; ++i)
    if (!parts[i].active) return i;
  return -1;
}

static void launchRocket() {
  for (int i = 0; i < MAX_ROCKETS; ++i) {
    if (!rockets[i].active) {
      rockets[i].active  = true;
      rockets[i].x       = random(40, axs::W - 40);
      rockets[i].y       = axs::H - 1;
      rockets[i].vy      = -(float)random(60, 95) / 10.0f;   // -6.0 .. -9.5
      rockets[i].targetY = random(60, axs::H / 2);           // top-ish band
      rndColor(rockets[i].r, rockets[i].g, rockets[i].b);
      return;
    }
  }
}

static void explode(float x, float y, uint8_t r, uint8_t g, uint8_t b) {
  int count = random(30, 41); // 30..40 sparks
  for (int n = 0; n < count; ++n) {
    int idx = findFreeParticle();
    if (idx < 0) return;
    float ang   = (float)random(0, 628) / 100.0f;     // 0..2pi
    float speed = (float)random(8, 38) / 10.0f;        // 0.8..3.8
    Particle &p = parts[idx];
    p.x      = x;
    p.y      = y;
    p.vx     = cosf(ang) * speed;
    p.vy     = sinf(ang) * speed;
    p.life   = 1.0f;
    p.decay  = (float)random(8, 20) / 1000.0f;         // ~0.008..0.020
    // slight per-spark color variation around the burst color
    int jr = (int)r + random(-30, 31);
    int jg = (int)g + random(-30, 31);
    int jb = (int)b + random(-30, 31);
    p.r = (uint8_t)(jr < 0 ? 0 : jr > 255 ? 255 : jr);
    p.g = (uint8_t)(jg < 0 ? 0 : jg > 255 ? 255 : jg);
    p.b = (uint8_t)(jb < 0 ? 0 : jb > 255 ? 255 : jb);
    p.active = true;
  }
}

static void app_setup() {
  Serial.println("[app045] fireworks");
  spr.setColorDepth(16);
  spr.setPsram(true);
  spr.createSprite(axs::W, axs::H);
  for (int i = 0; i < MAX_PARTICLES; ++i) parts[i].active = false;
  for (int i = 0; i < MAX_ROCKETS; ++i)   rockets[i].active = false;
}

static void app_loop() {
  static uint32_t frame = 0;
  spr.fillSprite(0x0000);

  // occasionally launch a new rocket
  if (random(100) < 7) launchRocket();

  // update + draw rockets
  for (int i = 0; i < MAX_ROCKETS; ++i) {
    Rocket &rk = rockets[i];
    if (!rk.active) continue;
    rk.y  += rk.vy;
    rk.vy += GRAVITY * 0.5f; // rockets slow as they climb
    // trailing glow
    spr.fillCircle((int)rk.x, (int)rk.y, 2, spr.color565(rk.r, rk.g, rk.b));
    spr.drawPixel((int)rk.x, (int)rk.y + 4, spr.color565(rk.r / 2, rk.g / 2, rk.b / 2));
    spr.drawPixel((int)rk.x, (int)rk.y + 7, spr.color565(rk.r / 3, rk.g / 3, rk.b / 3));
    // explode at target altitude or when it stalls
    if (rk.y <= rk.targetY || rk.vy >= -0.5f) {
      explode(rk.x, rk.y, rk.r, rk.g, rk.b);
      rk.active = false;
    }
  }

  // update + draw particles
  for (int i = 0; i < MAX_PARTICLES; ++i) {
    Particle &p = parts[i];
    if (!p.active) continue;
    p.x   += p.vx;
    p.y   += p.vy;
    p.vy  += GRAVITY;       // gravity pulls sparks down
    p.vx  *= 0.985f;        // slight air drag
    p.life -= p.decay;
    if (p.life <= 0.0f || p.y >= axs::H || p.x < 0 || p.x >= axs::W) {
      p.active = false;
      continue;
    }
    // fade color by life
    float    f  = p.life;
    uint8_t  cr = (uint8_t)(p.r * f);
    uint8_t  cg = (uint8_t)(p.g * f);
    uint8_t  cb = (uint8_t)(p.b * f);
    uint16_t col = spr.color565(cr, cg, cb);
    int xi = (int)p.x, yi = (int)p.y;
    // brighter, larger sparks while young
    if (p.life > 0.6f) {
      spr.fillCircle(xi, yi, 2, col);
    } else if (p.life > 0.3f) {
      spr.fillRect(xi, yi, 2, 2, col);
    } else {
      spr.drawPixel(xi, yi, col);
    }
  }

  ++frame;
  axs::push((const uint16_t*)spr.getBuffer());
  delay(16);
}
#endif
