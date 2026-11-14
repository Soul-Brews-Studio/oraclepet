#pragma once
// 008 sprite_continuous — push the same sprite as fast as possible, forever (no delay).
// LOOK FOR: a big white "8" on a MAGENTA screen, rock-steady (no flicker/white).
#if APP == 8
static LGFX_Sprite spr;

static void app_setup() {
  Serial.println("[app008] sprite_continuous — big '8', continuous push no delay");
  spr.setColorDepth(16);
  spr.setPsram(true);
  spr.createSprite(axs::W, axs::H);
  app_show(spr, "8", 0xF81F);  // magenta
}

static void app_loop() {
  axs::push((const uint16_t*)spr.getBuffer());  // no delay — stress the push loop
}
#endif
