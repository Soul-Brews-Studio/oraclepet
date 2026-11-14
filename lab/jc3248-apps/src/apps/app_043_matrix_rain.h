#pragma once
// 043 matrix_rain — falling green glyph columns with bright white heads and fading tails.
// LOOK FOR: ~20 vertical streams of green characters raining down a black screen, each
//           stream led by a glowing white head with a green trail that fades to black.
#if APP == 43
static LGFX_Sprite spr;

// ---- layout ----------------------------------------------------------------
static const int CELL = 16;                 // glyph cell size in pixels (text size 2 ~= 12x16)
static const int COLS = axs::W / CELL;       // 320/16 = 20 columns of glyphs
static const int ROWS = axs::H / CELL;       // 480/16 = 30 rows
static const int TAIL = 14;                  // length of the lit tail behind each head

// ---- per-column state ------------------------------------------------------
static float    col_y[COLS];                 // head position (fractional row)
static float    col_speed[COLS];             // rows advanced per frame
static int      col_len[COLS];               // visible tail length for this column
static char     glyph[COLS][ROWS];           // the character currently shown in each cell
static uint32_t glyph_seed = 1u;             // tiny LCG so glyphs flicker without random() spam

static inline char rand_glyph() {
  // printable ASCII band that reads "code-like": digits, letters, punctuation.
  glyph_seed = glyph_seed * 1664525u + 1013904223u;
  int r = (int)((glyph_seed >> 16) & 0x3F);  // 0..63
  static const char* set = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ<>*+=-/\\|{}[]#@%&$?";
  int n = 53;                                // length of set above
  return set[r % n];
}

static void reset_column(int c, bool topStart) {
  col_y[c]     = topStart ? -(float)(random(0, ROWS)) : 0.0f;
  col_speed[c] = 0.25f + (float)random(0, 60) * 0.01f;  // 0.25 .. 0.85 rows/frame
  col_len[c]   = 6 + random(0, TAIL - 5);               // 6 .. TAIL
}

static void app_setup() {
  Serial.println("[app043] matrix_rain");
  spr.setColorDepth(16);
  spr.setPsram(true);
  spr.createSprite(axs::W, axs::H);
  spr.setTextSize(2);
  spr.setTextDatum(textdatum_t::top_left);

  randomSeed(millis() ^ 0xC0FFEE);
  for (int c = 0; c < COLS; ++c) {
    reset_column(c, true);
    for (int r = 0; r < ROWS; ++r) glyph[c][r] = rand_glyph();
  }
}

static void app_loop() {
  static uint32_t frame = 0;
  ++frame;

  spr.fillSprite(0x0000);

  for (int c = 0; c < COLS; ++c) {
    int headRow = (int)col_y[c];
    int px = c * CELL;

    // draw the tail from the head upward
    for (int t = 0; t < col_len[c]; ++t) {
      int r = headRow - t;
      if (r < 0 || r >= ROWS) continue;

      int py = r * CELL;

      // occasionally mutate the glyph in lit cells so the stream shimmers
      if (((frame + (uint32_t)(c * 7 + r * 3)) & 0x07) == 0) glyph[c][r] = rand_glyph();

      char buf[2] = { glyph[c][r], 0 };

      if (t == 0) {
        // bright white-green head
        spr.setTextColor(0xFFFF);
      } else {
        // fade green: full at the head end, dark toward the tail
        int g = 255 - (t * 230) / col_len[c];   // 255 -> ~25
        if (g < 24) g = 24;
        spr.setTextColor(spr.color565(g >> 4, g, g >> 3));  // mostly green tint
      }
      spr.drawString(buf, px, py);
    }

    // advance the head
    col_y[c] += col_speed[c];

    // recycle when the tail has fully left the bottom
    if ((int)col_y[c] - col_len[c] > ROWS) {
      reset_column(c, false);
      col_y[c] = -(float)random(0, 8);  // re-enter just above the top with a little stagger
    }
  }

  axs::push((const uint16_t*)spr.getBuffer());
  delay(33);  // ~30fps target; the panel caps lower under DMA load
}
#endif
