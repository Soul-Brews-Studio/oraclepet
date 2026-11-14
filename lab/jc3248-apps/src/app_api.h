#pragma once
// ─────────────────────────────────────────────────────────────────────────
//  JC3248 App Gallery — the app contract.
//
//  Every app is one header `apps/app_<NNN>_<name>.h` that implements EXACTLY
//  two functions, guarded by its number so only the selected app compiles:
//
//      #if APP == <N>
//      static void app_setup() { /* one-time setup */ }
//      static void app_loop()  { /* render one frame, push, delay */ }
//      #endif
//
//  main.cpp already ran  Serial.begin(115200)  and  axs::init()  (the PROVEN
//  panel init + ~10s non-black colour warm-up), then cleared the screen to a
//  BLACK slate. The panel is READY when app_setup() runs — do NOT call
//  axs::init() again.
//
//  ── EVERY app must be self-identifying ──────────────────────────────────
//  The init() warm-up cycles colours for ~10s before any app runs, so an app
//  that merely "shows colours" is indistinguishable from the warm-up. Each app
//  therefore renders a BIG STATIC NUMBER (its own) on a distinct background via
//  app_show() — so at a glance you can read which app is running AND whether its
//  layer rendered. The FIRST app whose number does NOT appear (white/blank) is
//  the broken layer. app_000 is the only exception: it is the native colour
//  cycle baseline (no sprite, no font).
//
//  ── Native driver API (namespace axs, axs15231_qspi.h) ──────────────────
//    constexpr int axs::W = 320, axs::H = 480;       // panel size
//    void axs::push(const uint16_t* buf);            // push a W*H RGB565 frame
//    void axs::fill_solid(uint16_t wire);            // whole screen = one value
//                                                    //   wire = __builtin_bswap16(rgb565)
//    bool axs::read_touch(int16_t* x, int16_t* y);   // capacitive touch (0x3B)
//
//  ── LovyanGFX sprite (RGB565 framebuffer in PSRAM) ──────────────────────
//    LGFX_Sprite spr;                 // standalone — no parent device needed
//    spr.setColorDepth(16);           // REQUIRED: 16bpp RGB565
//    spr.setPsram(true);              // 320x480x2 = 307KB → PSRAM
//    spr.createSprite(axs::W, axs::H);
//    spr.fillSprite(0x0000); spr.fillRect(...); spr.drawString(...);
//    axs::push((const uint16_t*)spr.getBuffer());   // push the sprite to the panel
//
//  NOTE: LovyanGFX stores rgb565 sprites byte-swapped (MSB-first) already, so
//  axs::push of the raw sprite buffer is the correct wire order — no swap. Pass
//  NORMAL rgb565 (e.g. 0x07E0 green) to fillSprite/fillRect; LGFX swaps it.
//
//  ── AnimatedGIF (apps 010+) ─────────────────────────────────────────────
//    decode frames into the sprite via a GIFDraw callback, then axs::push.
//    GIF source: LittleFS /characters/bufo/*.gif (already flashed to the device).
// ─────────────────────────────────────────────────────────────────────────

// app_show — fill the sprite with a solid background and draw a big white,
// centred label, then push it. The uniform "self-identifying" render used by
// every numbered app: if you SEE the label, that app's render path works; if
// the screen is white/blank, that layer is the culprit.
//   bg565 = NORMAL rgb565 (LovyanGFX swaps it); label = the app's number text.
static inline void app_show(LGFX_Sprite& spr, const char* label, uint16_t bg565) {
  spr.fillSprite(bg565);
  spr.setTextColor(0xFFFF);
  spr.setTextDatum(textdatum_t::middle_center);
  spr.setTextSize(12);
  spr.drawString(label, axs::W / 2, axs::H / 2);
  axs::push((const uint16_t*)spr.getBuffer());
}
