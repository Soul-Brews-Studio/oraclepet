/*
 * touch.h — waveshare-2.8b-pet stub (LCD-only, no touch panel)
 *
 * The Waveshare 2.8B is the non-touch variant. touch_init() always succeeds;
 * touch_poll() always returns false. API matches jc3248-pet-idf touch.h so
 * main.c compiles unchanged.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t touch_init(void);
bool touch_poll(uint16_t *x, uint16_t *y);
