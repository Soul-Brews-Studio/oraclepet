/*
 * chaiklang_host.c — run PR #1's chaiklang.wasm on the ESP32-S3 via WAMR.
 *
 * The wasm exports lion_pulse(i32)->i32 and route(i32,i32)->i32, zero imports.
 * Same WAMR host pattern as gif_wamr_main.c but stripped to the minimum.
 * Expected output: lion_pulse(100)=5050, route(3,4)=15.
 */
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wasm_export.h"

static const char *TAG = "chaiklang";

#define WASM_STACK   (16 * 1024)
#define PTH_STACK    (16 * 1024)

extern const uint8_t ck_wasm_start[] asm("_binary_chaiklang_wasm_start");
extern const uint8_t ck_wasm_end[]   asm("_binary_chaiklang_wasm_end");

static void run_chaiklang(void)
{
    uint32_t sz = (uint32_t)(ck_wasm_end - ck_wasm_start);
    ESP_LOGI(TAG, "=== ChaiKlang on ESP32-S3 via WAMR === wasm=%u bytes", (unsigned)sz);

    RuntimeInitArgs init = {0};
    init.mem_alloc_type = Alloc_With_System_Allocator;
    if (!wasm_runtime_full_init(&init)) { ESP_LOGE(TAG, "init failed"); return; }

    char err[128];
    uint8_t *buf = heap_caps_malloc(sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) { ESP_LOGE(TAG, "no RAM for wasm copy"); return; }
    memcpy(buf, ck_wasm_start, sz);

    wasm_module_t mod = wasm_runtime_load(buf, sz, err, sizeof(err));
    if (!mod) { ESP_LOGE(TAG, "load: %s", err); heap_caps_free(buf); return; }

    wasm_module_inst_t inst = wasm_runtime_instantiate(mod, WASM_STACK, 0, err, sizeof(err));
    if (!inst) { ESP_LOGE(TAG, "inst: %s", err); wasm_runtime_unload(mod); heap_caps_free(buf); return; }

    wasm_exec_env_t env = wasm_runtime_create_exec_env(inst, WASM_STACK);
    if (!env) { ESP_LOGE(TAG, "env failed"); goto done; }

    /* call lion_pulse(100) — expect 5050 */
    wasm_function_inst_t f_pulse = wasm_runtime_lookup_function(inst, "lion_pulse");
    if (f_pulse) {
        uint32_t argv[1] = {100};
        if (wasm_runtime_call_wasm(env, f_pulse, 1, argv))
            ESP_LOGI(TAG, "lion_pulse(100) = %u   (expect 5050) %s", argv[0], argv[0]==5050?"PASS":"FAIL");
        else
            ESP_LOGE(TAG, "lion_pulse trapped: %s", wasm_runtime_get_exception(inst));
    }

    /* call route(3,4) — expect 15 */
    wasm_function_inst_t f_route = wasm_runtime_lookup_function(inst, "route");
    if (f_route) {
        uint32_t argv[2] = {3, 4};
        if (wasm_runtime_call_wasm(env, f_route, 2, argv))
            ESP_LOGI(TAG, "route(3,4) = %u        (expect 15) %s", argv[0], argv[0]==15?"PASS":"FAIL");
        else
            ESP_LOGE(TAG, "route trapped: %s", wasm_runtime_get_exception(inst));
    }

    ESP_LOGI(TAG, "ChaiKlang wasm ran on the ESP32-S3 (WAMR). The Middle Switchboard, on-chip.");
    wasm_runtime_destroy_exec_env(env);
done:
    wasm_runtime_deinstantiate(inst);
    wasm_runtime_unload(mod);
    heap_caps_free(buf);
    wasm_runtime_destroy();
}

static void *wamr_thread(void *arg) { run_chaiklang(); return NULL; }

void app_main(void)
{
    ESP_LOGI(TAG, "== ChaiKlang Oracle — wasm-on-ESP32-S3 (WAMR, PR #1 test) ==");
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, PTH_STACK);
    pthread_t th;
    pthread_create(&th, &attr, wamr_thread, NULL);
    pthread_join(th, NULL);
    ESP_LOGI(TAG, "done — looping");
    while (1) vTaskDelay(pdMS_TO_TICKS(2000));
}
