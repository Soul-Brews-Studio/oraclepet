#pragma once
// 003 sprite_fastpush — push the same sprite continuously with no delay (push-rate stress).
// LOOK FOR: a big white "3" on a PURPLE screen, rock-steady (no tearing/flicker/white).
#if APP == 3
static LGFX_Sprite spr;

static void app_setup() {
  Serial.println("[app003] sprite_fastpush — big '3' pushed continuously (no delay)");
  spr.setColorDepth(16);
  spr.setPsram(true);
  spr.createSprite(axs::W, axs::H);
  app_show(spr, "3", 0x8010);  // purple
}

static void app_loop() {
  axs::push((const uint16_t*)spr.getBuffer());  // tight push, no delay
}
#endif
