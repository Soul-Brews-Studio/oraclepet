#pragma once
// 005 tft_setrotation — call dev.setRotation(0) (a LovyanGFX SPI2 bus op) then push.
// This reproduces the buddy's M5.Lcd.setRotation(0) — prime suspect for clobbering
// the native driver that owns SPI2.
// LOOK FOR: a big white "5" on an ORANGE screen. (white/blank = setRotation is the breaker)
#if APP == 5

#include "../lgfx_jc3248_device.h"

static LGFX_JC3248 dev;
static LGFX_Sprite spr;

static void app_setup() {
  Serial.println("[app005] tft_setrotation — dev.setRotation(0) then big '5'");
  dev.setRotation(0);  // SPI2 bus op on the un-init device — does it clobber native?
  spr.setColorDepth(16);
  spr.setPsram(true);
  spr.createSprite(axs::W, axs::H);
  app_show(spr, "5", 0xFD20);  // orange
}

static void app_loop() {
  app_show(spr, "5", 0xFD20);
  delay(500);
}
#endif
