#pragma once
// 004 sprite_parented — sprite parented to an (un-init'd) LGFX_JC3248 device on SPI2.
// Does merely constructing/parenting a LovyanGFX device break the native push?
// LOOK FOR: a big white "4" on a CYAN screen. (white/blank = parenting breaks rendering)
#if APP == 4

#include "../lgfx_jc3248_device.h"

static LGFX_JC3248 dev;
static LGFX_Sprite spr(&dev);

static void app_setup() {
  Serial.println("[app004] sprite_parented — sprite parented to un-init LGFX_JC3248; big '4'");
  spr.setColorDepth(16);
  spr.setPsram(true);
  spr.createSprite(axs::W, axs::H);
  app_show(spr, "4", 0x07FF);  // cyan
}

static void app_loop() {
  app_show(spr, "4", 0x07FF);
  delay(500);
}
#endif
