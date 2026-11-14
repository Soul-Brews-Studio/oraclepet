#if defined(BOARD_LILYGO_EPD47)
#include "hal_epd47.h"
#include "epd_font_data.h"

int epd_driver_v6_vcom = 1560;

HalEpd47 tft;
HalM5 M5(tft);

void EpdSprite::_drawChar(char c, int x, int y, uint8_t fg, uint8_t bg, uint8_t sz) {
  if (c < 0x20 || c > 0x7E) return;
  int idx = (c - 0x20) * 5;
  for (int col = 0; col < 5; col++) {
    uint8_t line = epd_font_5x7[idx + col];
    for (int row = 0; row < 8; row++) {
      uint8_t g = (line & (1 << row)) ? fg : bg;
      if (sz == 1) {
        _setPixel(x + col, y + row, g);
      } else {
        for (int sy = 0; sy < sz; sy++)
          for (int sx = 0; sx < sz; sx++)
            _setPixel(x + col * sz + sx, y + row * sz + sy, g);
      }
    }
  }
  // Spacing column
  for (int row = 0; row < 8 * sz; row++)
    for (int sx = 0; sx < sz; sx++)
      _setPixel(x + 5 * sz + sx, y + row, bg);
}

#endif
