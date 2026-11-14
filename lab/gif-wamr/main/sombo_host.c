/*
 * sombo_host.c — hardware-verify workshop PR #4 (88-sombo) on the ESP32-S3 via WAMR.
 * gifcore.wasm exports: add(i32,i32), selftest()->6291556, gif_load(off,byte),
 * gif_parse()->(w<<16)|h, gif_width(), gif_height(). Zero imports.
 * Test: add(2,3)=5, selftest=6291556, then feed a GIF89a 96x100 header and parse it.
 *
 *   idf.py -DWAMR_HOST=sombo build flash monitor
 */
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wasm_export.h"

static const char *TAG = "sombo";

#define WASM_STACK   (16 * 1024)
#define PTH_STACK    (16 * 1024)

extern const uint8_t sombo_wasm_start[] asm("_binary_gifcore_wasm_start");
extern const uint8_t sombo_wasm_end[]   asm("_binary_gifcore_wasm_end");

static uint32_t call1(wasm_exec_env_t env, wasm_module_inst_t inst, const char *name,
                      uint32_t argc, uint32_t *argv)
{
    wasm_function_inst_t f = wasm_runtime_lookup_function(inst, name);
    if (!f) { ESP_LOGE(TAG, "export '%s' not found", name); return 0xFFFFFFFF; }
    if (!wasm_runtime_call_wasm(env, f, argc, argv)) {
        ESP_LOGE(TAG, "call '%s' trapped: %s", name, wasm_runtime_get_exception(inst));
        return 0xFFFFFFFF;
    }
    return argv[0];
}

static void run_sombo(void)
{
    uint32_t sz = (uint32_t)(sombo_wasm_end - sombo_wasm_start);
    ESP_LOGI(TAG, "=== 88-sombo GIF parser on ESP32-S3 via WAMR === wasm=%u bytes", (unsigned)sz);

    RuntimeInitArgs init = {0};
    init.mem_alloc_type = Alloc_With_System_Allocator;
    if (!wasm_runtime_full_init(&init)) { ESP_LOGE(TAG, "init failed"); return; }

    char err[128];
    uint8_t *buf = heap_caps_malloc(sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) { ESP_LOGE(TAG, "no RAM for wasm copy"); return; }
    memcpy(buf, sombo_wasm_start, sz);

    wasm_module_t mod = wasm_runtime_load(buf, sz, err, sizeof(err));
    if (!mod) { ESP_LOGE(TAG, "load: %s", err); heap_caps_free(buf); return; }
    wasm_module_inst_t inst = wasm_runtime_instantiate(mod, WASM_STACK, 0, err, sizeof(err));
    if (!inst) { ESP_LOGE(TAG, "inst: %s", err); wasm_runtime_unload(mod); heap_caps_free(buf); return; }
    wasm_exec_env_t env = wasm_runtime_create_exec_env(inst, WASM_STACK);
    if (!env) { ESP_LOGE(TAG, "env failed"); goto done; }

    /* add(2,3) — expect 5 */
    { uint32_t a[2] = {2, 3}; uint32_t r = call1(env, inst, "add", 2, a);
      ESP_LOGI(TAG, "add(2,3) = %u   (expect 5) %s", r, r==5?"PASS":"FAIL"); }

    /* selftest() — expect 6291556 (96<<16|100) */
    { uint32_t a[1] = {0}; uint32_t r = call1(env, inst, "selftest", 0, a);
      ESP_LOGI(TAG, "selftest() = %u   (expect 6291556) %s", r, r==6291556?"PASS":"FAIL"); }

    /* feed a GIF89a 96x100 header, then parse on-chip */
    const uint8_t hdr[13] = {'G','I','F','8','9','a', 0x60,0x00, 0x64,0x00, 0xF7,0x00,0x00};
    wasm_function_inst_t f_load = wasm_runtime_lookup_function(inst, "gif_load");
    for (uint32_t i = 0; i < 13; i++) { uint32_t a[2] = {i, hdr[i]}; wasm_runtime_call_wasm(env, f_load, 2, a); }
    { uint32_t a[1] = {0}; uint32_t r = call1(env, inst, "gif_parse", 0, a);
      ESP_LOGI(TAG, "gif_parse() = %u   (expect 6291556) %s", r, r==6291556?"PASS":"FAIL"); }
    { uint32_t a[1] = {0}; uint32_t w = call1(env, inst, "gif_width", 0, a);
      uint32_t b[1] = {0}; uint32_t h = call1(env, inst, "gif_height", 0, b);
      ESP_LOGI(TAG, "gif_width=%u gif_height=%u   (expect 96x100) %s", w, h, (w==96&&h==100)?"PASS":"FAIL"); }

    ESP_LOGI(TAG, "88-sombo gifcore.wasm ran on the ESP32-S3 (WAMR). GIF parsed on silicon.");
    wasm_runtime_destroy_exec_env(env);
done:
    wasm_runtime_deinstantiate(inst);
    wasm_runtime_unload(mod);
    heap_caps_free(buf);
    wasm_runtime_destroy();
}

static void *wamr_thread(void *arg) { run_sombo(); return NULL; }

void app_main(void)
{
    ESP_LOGI(TAG, "== workshop PR #4 (88-sombo) — wasm-on-ESP32-S3 (WAMR) ==");
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, PTH_STACK);
    pthread_t th;
    pthread_create(&th, &attr, wamr_thread, NULL);
    pthread_join(th, NULL);
    ESP_LOGI(TAG, "done — looping");
    while (1) vTaskDelay(pdMS_TO_TICKS(2000));
}
