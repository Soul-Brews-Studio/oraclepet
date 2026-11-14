#pragma once
// 002 sprite_shapes — LGFX_Sprite primitives: fillRect, drawRect, drawString.
// LOOK FOR: two bordered rectangles (red, green) and a big white "2" on black.
#if APP == 2
static LGFX_Sprite spr;

static void app_setup() {
  Serial.println("[app002] sprite_shapes — fillRect/drawRect + big '2'");
  spr.setColorDepth(16);
  spr.setPsram(true);
  spr.createSprite(axs::W, axs::H);
}

static void app_loop() {
  spr.fillSprite(0x0000);
  spr.fillRect(40, 40, 100, 80, 0xF800);    // red
  spr.fillRect(180, 40, 100, 80, 0x07E0);   // green
  spr.drawRect(40, 40, 100, 80, 0xFFFF);    // white borders
  spr.drawRect(180, 40, 100, 80, 0xFFFF);
  spr.setTextColor(0xFFFF);
  spr.setTextDatum(textdatum_t::middle_center);
  spr.setTextSize(12);
  spr.drawString("2", axs::W / 2, axs::H / 2 + 40);
  axs::push((const uint16_t*)spr.getBuffer());
  delay(500);
}
#endif
