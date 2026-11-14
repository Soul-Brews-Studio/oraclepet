/*
 * idf_compat.h — Arduino-shim for the vendored AnimatedGIF under ESP-IDF.
 *
 * AnimatedGIF.cpp::playFrame() calls Arduino's millis()/delay() inside the
 *   #if !defined(__MACH__) && !defined(__LINUX__)
 * block (the bSync==true pacing path). Under ESP-IDF neither macro is defined,
 * so that code is compiled — but Arduino.h is NOT present, so millis()/delay()
 * are undeclared and the build fails. We never call playFrame(bSync=true)
 * (gif.cpp always passes false, and paces frames itself via esp_timer), so this
 * path is never *executed*; it only needs to *compile*.
 *
 * Rather than patch the vendored sources (keeping them byte-identical to
 * upstream bitbank2/AnimatedGIF for easy re-vendoring), we force-include this
 * header into every AnimatedGIF translation unit via -include in the component
 * CMakeLists. It maps the two Arduino calls onto IDF primitives.
 */
#pragma once

#ifndef ARDUINO   /* only when not building under the Arduino core */

#include <stdint.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static inline uint32_t millis(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static inline void delay(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

#endif /* !ARDUINO */
