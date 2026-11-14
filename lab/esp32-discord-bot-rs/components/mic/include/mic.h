/* mic.h — the pet board's ES8311 mic, for Rust (bindings in esp_idf_svc::sys::mic). */
#pragma once
#include <stdint.h>

/* Bring up I2C, duplex I2S0 and the ES8311 at 16 kHz mono. 0 on success. */
int mic_start(void);
/* Block until `samples` 16-bit samples are read (20 ms = 320). 0 on success. */
int mic_read(int16_t *pcm, int samples);
