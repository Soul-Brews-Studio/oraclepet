#pragma once
// 001 sprite_solid — the simplest LovyanGFX sprite → axs::push path.
// LOOK FOR: a big white "1" on a GREEN screen, static. (white/blank = sprite path broken)
#if APP == 1
static LGFX_Sprite spr;

static void app_setup() {
  Serial.println("[app001] sprite_solid — big static '1' on green");
  spr.setColorDepth(16);
  spr.setPsram(true);
  spr.createSprite(axs::W, axs::H);
  app_show(spr, "1", 0x07E0);  // green
}

static void app_loop() {
  app_show(spr, "1", 0x07E0);
  delay(500);
}
#endif
