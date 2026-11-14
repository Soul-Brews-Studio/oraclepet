#pragma once
// 009 sprite_doublebuffer — two PSRAM sprites, push alternately (ping-pong).
// LOOK FOR: a big white "9" on a MAROON screen, steady (both buffers render the same).
#if APP == 9
static LGFX_Sprite spr0, spr1;
static uint32_t f = 0;

static void app_setup() {
  Serial.println("[app009] sprite_doublebuffer — two PSRAM sprites, big '9'");
  spr0.setColorDepth(16);
  spr0.setPsram(true);
  spr0.createSprite(axs::W, axs::H);
  spr1.setColorDepth(16);
  spr1.setPsram(true);
  spr1.createSprite(axs::W, axs::H);
}

static void app_loop() {
  LGFX_Sprite& cur = (f++ & 1) ? spr1 : spr0;
  app_show(cur, "9", 0x8000);  // maroon
  delay(200);
}
#endif
