#pragma once
// 006 tft_setbrightness — call dev.setBrightness(200) (LEDC backlight on GPIO1) then push.
// Does driving the LovyanGFX PWM backlight disrupt the native axs driver?
// LOOK FOR: a big white "6" on a BLUE screen. (white/blank = setBrightness is the breaker)
#if APP == 6

#include "../lgfx_jc3248_device.h"

static LGFX_JC3248 dev;
static LGFX_Sprite spr;

static void app_setup() {
  Serial.println("[app006] tft_setbrightness — dev.setBrightness(200) then big '6'");
  dev.setBrightness(200);  // LEDC on GPIO1 — does it disrupt native?
  spr.setColorDepth(16);
  spr.setPsram(true);
  spr.createSprite(axs::W, axs::H);
  app_show(spr, "6", 0x001F);  // blue
}

static void app_loop() {
  app_show(spr, "6", 0x001F);
  delay(500);
}
#endif
