/* mic.h — the pet board's ES8311 (I2C 16/15, I2S0 4/5/7/8/6, amp GPIO1) as an esp_codec_dev. */
#pragma once
#include "esp_codec_dev.h"

/* Bring up I2C, duplex I2S0 and the ES8311. NULL on failure. Opened by esp_capture later. */
esp_codec_dev_handle_t mic_open(void);
