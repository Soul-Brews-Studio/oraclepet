#pragma once
// 000 colorcycle — native driver only (PROVEN baseline).
// Cycles full-screen solid colours forever via axs::fill_solid (no LovyanGFX,
// no sprite, no GIF). If this works, the native QSPI driver renders + updates
// fine post-init; every later app adds exactly one layer on top.
// LOOK FOR: red → green → blue → yellow → white, ~1s each, forever.
#if APP == 0
static void app_setup() {
  Serial.println("[app000] colorcycle — native driver only");
}
static void app_loop() {
  static const uint16_t rgb[] = {0xF800, 0x07E0, 0x001F, 0xFFE0, 0xFFFF};
  for (uint16_t c : rgb) {
    axs::fill_solid(__builtin_bswap16(c));   // MSB-first wire order
    delay(1000);
  }
}
#endif
