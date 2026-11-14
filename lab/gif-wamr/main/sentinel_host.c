/*
 * sentinel_host.c — run the esp32-wasm workshop submission's sentinel.wasm on
 * the ESP32-S3 via WAMR. The wasm exports flame(i32)->i32 and sense(i32,i32)->i32,
 * zero imports. Same WAMR host pattern as gif_wamr_main.c, stripped to the minimum.
 * Expected output: flame(10)=385, sense(3,4)=25.
 *
 *   idf.py -DWAMR_HOST=sentinel build flash monitor
 */
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wasm_export.h"

static const char *TAG = "sentinel";

#define WASM_STACK   (16 * 1024)
#define PTH_STACK    (16 * 1024)

extern const uint8_t sn_wasm_start[] asm("_binary_sentinel_wasm_start");
extern const uint8_t sn_wasm_end[]   asm("_binary_sentinel_wasm_end");

static void run_sentinel(void)
{
    uint32_t sz = (uint32_t)(sn_wasm_end - sn_wasm_start);
    ESP_LOGI(TAG, "=== ESP32 Oracle Sentinel on ESP32-S3 via WAMR === wasm=%u bytes", (unsigned)sz);

    RuntimeInitArgs init = {0};
    init.mem_alloc_type = Alloc_With_System_Allocator;
    if (!wasm_runtime_full_init(&init)) { ESP_LOGE(TAG, "init failed"); return; }

    char err[128];
    uint8_t *buf = heap_caps_malloc(sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) { ESP_LOGE(TAG, "no RAM for wasm copy"); return; }
    memcpy(buf, sn_wasm_start, sz);

    wasm_module_t mod = wasm_runtime_load(buf, sz, err, sizeof(err));
    if (!mod) { ESP_LOGE(TAG, "load: %s", err); heap_caps_free(buf); return; }

    wasm_module_inst_t inst = wasm_runtime_instantiate(mod, WASM_STACK, 0, err, sizeof(err));
    if (!inst) { ESP_LOGE(TAG, "inst: %s", err); wasm_runtime_unload(mod); heap_caps_free(buf); return; }

    wasm_exec_env_t env = wasm_runtime_create_exec_env(inst, WASM_STACK);
    if (!env) { ESP_LOGE(TAG, "env failed"); goto done; }

    /* call flame(10) — expect 385 (1²+2²+...+10²) */
    wasm_function_inst_t f_flame = wasm_runtime_lookup_function(inst, "flame");
    if (f_flame) {
        uint32_t argv[1] = {10};
        if (wasm_runtime_call_wasm(env, f_flame, 1, argv))
            ESP_LOGI(TAG, "flame(10) = %u   (expect 385) %s", argv[0], argv[0]==385?"PASS":"FAIL");
        else
            ESP_LOGE(TAG, "flame trapped: %s", wasm_runtime_get_exception(inst));
    }

    /* call sense(3,4) — expect 25 (9+16) */
    wasm_function_inst_t f_sense = wasm_runtime_lookup_function(inst, "sense");
    if (f_sense) {
        uint32_t argv[2] = {3, 4};
        if (wasm_runtime_call_wasm(env, f_sense, 2, argv))
            ESP_LOGI(TAG, "sense(3,4) = %u        (expect 25) %s", argv[0], argv[0]==25?"PASS":"FAIL");
        else
            ESP_LOGE(TAG, "sense trapped: %s", wasm_runtime_get_exception(inst));
    }

    ESP_LOGI(TAG, "Sentinel wasm ran on the ESP32-S3 (WAMR). Small body, holding the flame.");
    wasm_runtime_destroy_exec_env(env);
done:
    wasm_runtime_deinstantiate(inst);
    wasm_runtime_unload(mod);
    heap_caps_free(buf);
    wasm_runtime_destroy();
}

static void *wamr_thread(void *arg) { run_sentinel(); return NULL; }

void app_main(void)
{
    ESP_LOGI(TAG, "== ESP32 Oracle — wasm-on-ESP32-S3 (WAMR, sentinel submission) ==");
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, PTH_STACK);
    pthread_t th;
    pthread_create(&th, &attr, wamr_thread, NULL);
    pthread_join(th, NULL);
    ESP_LOGI(TAG, "done — looping");
    while (1) vTaskDelay(pdMS_TO_TICKS(2000));
}
