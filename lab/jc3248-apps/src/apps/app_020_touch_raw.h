#pragma once
// 020 touch_raw — capacitive touch read (axs::read_touch); coords go to serial.
// LOOK FOR: a big white "20" on a NAVY screen; `make monitor` prints (x,y) when you touch.
#if APP == 20
static LGFX_Sprite spr;

static void app_setup() {
  Serial.println("[app020] touch_raw — big '20' on navy; serial prints touch coords");
  spr.setColorDepth(16);
  spr.setPsram(true);
  spr.createSprite(axs::W, axs::H);
  app_show(spr, "20", 0x000F);  // navy
}

static void app_loop() {
  int16_t x, y;
  if (axs::read_touch(&x, &y)) {
    Serial.printf("[app020] touch (%d,%d)\n", x, y);
  }
  delay(30);
}
#endif
