#if defined(BOARD_ATD35_S3)
#include "hal.h"

#if defined(BOARD_WAVESHARE_2_8B)
LGFX_WS2_8B tft;
#else
LGFX_ATD35 tft;
#endif
HalM5 M5(tft);

#endif
