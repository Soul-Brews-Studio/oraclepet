#pragma once
// 041 starfield — warp-speed 3D starfield streaming outward from center.
// LOOK FOR: ~150 pale stars rushing from the middle of the screen toward the edges,
//           accelerating and growing as they near you, then respawning at center.
#if APP == 41
static LGFX_Sprite spr;

static const int STAR_COUNT = 150;
struct Star {
  float x, y, z;     // x,y in [-1,1] plane units; z = depth (near=small, far=big)
  float pz;          // previous z, for motion-trail streaks
};
static Star stars[STAR_COUNT];

static const float Z_FAR  = 1.0f;   // spawn depth
static const float Z_NEAR = 0.02f;  // closest before respawn
static const float SPEED  = 0.015f; // how fast z shrinks per frame (warp speed)

// Seed one star at a random angle/depth, originating near center.
static void respawn_star(Star &s) {
  // random direction on the projection plane
  float ang = (float)random(0, 6283) / 1000.0f;  // 0..2*PI
  float rad = (float)random(2, 1000) / 1000.0f;  // small offset so it streams outward
  s.x = cosf(ang) * rad;
  s.y = sinf(ang) * rad;
  s.z = Z_FAR;
  s.pz = Z_FAR;
}

static void app_setup() {
  Serial.println("[app041] starfield");
  spr.setColorDepth(16);
  spr.setPsram(true);
  spr.createSprite(axs::W, axs::H);
  for (int i = 0; i < STAR_COUNT; i++) {
    // stagger initial depths so the field fills immediately
    respawn_star(stars[i]);
    stars[i].z = (float)random(20, 1000) / 1000.0f;
    stars[i].pz = stars[i].z;
  }
}

static void app_loop() {
  static uint32_t frame = 0;
  frame++;

  spr.fillSprite(0x0000);  // black space

  const float cx = axs::W * 0.5f;
  const float cy = axs::H * 0.5f;
  // focal length scaled so stars spread across the full panel as they approach
  const float focal = (float)axs::H * 0.9f;

  // slow color drift so the field shifts hue over time (cool blue-white -> warm white)
  float t = frame * 0.01f;
  int tintR = 200 + (int)(40.0f * sinf(t));
  int tintG = 200 + (int)(40.0f * sinf(t + 2.094f));
  int tintB = 220 + (int)(35.0f * sinf(t + 4.188f));

  for (int i = 0; i < STAR_COUNT; i++) {
    Star &s = stars[i];

    s.pz = s.z;
    s.z -= SPEED;  // move toward the viewer (warp)

    if (s.z <= Z_NEAR) {
      respawn_star(s);
      continue;
    }

    // 3D perspective projection: divide plane coords by depth
    float inv  = focal / s.z;
    float sx   = cx + s.x * inv;
    float sy   = cy + s.y * inv;

    // previous-frame projection for the streak tail
    float invp = focal / s.pz;
    float px   = cx + s.x * invp;
    float py   = cy + s.y * invp;

    // off-screen? respawn at center with a fresh angle
    if (sx < 0 || sx >= axs::W || sy < 0 || sy >= axs::H) {
      respawn_star(s);
      continue;
    }

    // brightness & size grow as z shrinks (closer = bigger & brighter)
    float near = (Z_FAR - s.z) / (Z_FAR - Z_NEAR);  // 0 far .. 1 near
    if (near < 0) near = 0;
    if (near > 1) near = 1;

    int r = (int)(tintR * (0.25f + 0.75f * near));
    int g = (int)(tintG * (0.25f + 0.75f * near));
    int b = (int)(tintB * (0.25f + 0.75f * near));
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    uint16_t col = spr.color565(r, g, b);

    // streak from previous to current position for a warp-trail feel
    spr.drawLine((int)px, (int)py, (int)sx, (int)sy, col);

    // star head: size scales with proximity
    int rad = (int)(near * 3.0f);
    if (rad <= 0) {
      spr.drawPixel((int)sx, (int)sy, col);
    } else {
      spr.fillCircle((int)sx, (int)sy, rad, col);
    }
  }

  axs::push((const uint16_t*)spr.getBuffer());
  delay(16);  // ~60fps target; panel push caps real fps lower
}
#endif
