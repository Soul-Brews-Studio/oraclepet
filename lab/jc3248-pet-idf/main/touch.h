#pragma once
/*
 * jc3248-pet-idf — STEP 4: AXS15231 capacitive touch (touch.h)
 *
 * I2C 0x3B on SDA=GPIO4 / SCL=GPIO8, INT on GPIO3 (active-low / NEGEDGE).
 * INT-GATED: reads happen ONLY after a real touch interrupt edge — the vendor
 * esp_lcd_touch read_data blind-polls, which on the Arduino body produced
 * phantom taps that auto-cycled the brightness. Here the component's
 * interrupt_callback just latches a flag; touch_poll() reads I2C only when it's
 * set. At rest, touch_poll() never fires.
 */
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Bring up the i2c_master bus + AXS15231 touch panel. Call after display_init(). */
esp_err_t touch_init(void);

/* Non-blocking. Returns true + fills *x,*y only on a fresh INT-gated touch. */
bool touch_poll(uint16_t *x, uint16_t *y);
