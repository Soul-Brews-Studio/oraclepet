#pragma once
// 021 touch_draw — draw yellow dots at the touch point (touch → sprite → push).
// LOOK FOR: a big white "21" on black; yellow dots/trail follow your finger.
#if APP == 21
static LGFX_Sprite spr;

static void app_setup() {
  Serial.println("[app021] touch_draw — '21'; yellow dots follow your finger");
  spr.setColorDepth(16);
  spr.setPsram(true);
  spr.createSprite(axs::W, axs::H);
  app_show(spr, "21", 0x0000);  // black, with the "21" label
}

static void app_loop() {
  int16_t x, y;
  if (axs::read_touch(&x, &y)) {
    spr.fillCircle(x, y, 8, 0xFFE0);  // yellow dot
    axs::push((const uint16_t*)spr.getBuffer());
  }
  delay(16);
}
#endif
