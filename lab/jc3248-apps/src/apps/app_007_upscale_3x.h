#pragma once
// 007 upscale_3x — 32x32 checker upscaled 3x via fillRect blocks (the GIF pet's scaler).
// LOOK FOR: a blocky 96x96 yellow/blue checker with a big white "7" below it, static.
#if APP == 7
static LGFX_Sprite spr;

static void app_setup() {
  Serial.println("[app007] upscale_3x — 32x32 checker upscaled 3x + big '7'");
  spr.setColorDepth(16);
  spr.setPsram(true);
  spr.createSprite(axs::W, axs::H);
}

static void app_loop() {
  spr.fillSprite(0x0000);
  const int S = 3, N = 32;
  const int ux = (axs::W - N * S) / 2, uy = 100;
  for (int y = 0; y < N; y++) {
    for (int x = 0; x < N; x++) {
      uint16_t c = ((x + y) & 1) ? 0xFFE0 : 0x001F;  // yellow / blue
      spr.fillRect(ux + x * S, uy + y * S, S, S, c);
    }
  }
  spr.setTextColor(0xFFFF);
  spr.setTextDatum(textdatum_t::middle_center);
  spr.setTextSize(12);
  spr.drawString("7", axs::W / 2, axs::H - 90);
  axs::push((const uint16_t*)spr.getBuffer());
  delay(500);
}
#endif
