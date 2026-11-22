/*
 * qemu_demo.c — qemu board only: a demo DustBoy Kit feed (see qemu_demo.h).
 *
 * Two kits: "173e5c" walks through good / fair / poor PM2.5 values so every HUD band colour
 * shows within a minute or two; "a1b2c3" stays good. Readings are tagged as BLE, the source
 * that feeds dbk_on_reading() on the real board.
 */
#include "qemu_demo.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "dbk.h"

static const char *TAG = "qemu-demo";

static void demo_task(void *arg)
{
    static const int pm25[] = { 12, 18, 24, 31, 44, 57, 63, 48, 36, 21 };
    const int n = sizeof pm25 / sizeof pm25[0];
    for (int i = 0;; i = (i + 1) % n) {
        int a = pm25[i], b = 8 + (i % 3);
        dbk_on_reading("173e5c", a, a + a / 2, a / 2, dbk_band_of(a), -48, DBK_SRC_BLE);
        dbk_on_reading("a1b2c3", b, b + 4, b / 2, dbk_band_of(b), -63, DBK_SRC_BLE);
        ESP_LOGI(TAG, "demo kit readings: 173e5c pm25=%d (%s), a1b2c3 pm25=%d", a, dbk_band_name(dbk_band_of(a)), b);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void qemu_demo_start(void)
{
    xTaskCreate(demo_task, "qemu_demo", 3072, NULL, 3, NULL);
}
