/*
 * smoke_all_host.c — flash-test EVERY workshop submission's wasm on the ESP32-S3
 * in ONE firmware. For each embedded module: load under WAMR, enumerate its
 * exports, call every function export with zero-args (i32), and report
 * loaded?/ran-count. One flash proves all submissions run on real silicon.
 *
 *   idf.py -DWAMR_HOST=smoke build flash monitor
 *
 * Add a module: drop its .wasm as main/prNN.wasm, EMBED_FILES it in
 * main/CMakeLists.txt, and add an X(...) line to PR_WASMS below.
 */
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wasm_export.h"

static const char *TAG = "smoke";

/* ── the module table (label, embed-symbol-stem) ───────────────────────────
 * symbol stem must match the embedded filename: prNN.wasm → _binary_prNN_wasm_* */
#define PR_WASMS \
    X("PR#5  03-singhasingha", pr05) \
    X("PR#6  88-sombo-gifapp", pr06) \
    X("PR#7  03-gon-aqi",      pr07) \
    X("PR#8  03-tonk",         pr08) \
    X("PR#9  03-bongbaeng",    pr09) \
    X("PR#10 03-lord-knight",  pr10) \
    X("PR#11 03-leica-calc",   pr11) \
    X("PR#12 03-vessel",       pr12) \
    X("PR#14 05-vialumen",     pr14)

/* extern decls for each embedded blob */
#define X(label, sym) \
    extern const uint8_t sym##_start[] asm("_binary_" #sym "_wasm_start"); \
    extern const uint8_t sym##_end[]   asm("_binary_" #sym "_wasm_end");
PR_WASMS
#undef X

typedef struct { const char *label; const uint8_t *s; const uint8_t *e; } wentry_t;
static const wentry_t TABLE[] = {
#define X(label, sym) { label, sym##_start, sym##_end },
PR_WASMS
#undef X
};
#define N_MODULES (sizeof(TABLE) / sizeof(TABLE[0]))

#define WSTACK (16 * 1024)

/* load one module, enumerate + call its function exports, return ran/funcs */
static void test_one(const wentry_t *m)
{
    uint32_t sz = (uint32_t)(m->e - m->s);
    char err[128];
    uint8_t *buf = heap_caps_malloc(sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) { ESP_LOGE(TAG, "[%s] no RAM (%u B)", m->label, (unsigned)sz); return; }
    memcpy(buf, m->s, sz);

    wasm_module_t mod = wasm_runtime_load(buf, sz, err, sizeof(err));
    if (!mod) { ESP_LOGE(TAG, "[%s] LOAD FAIL: %s", m->label, err); heap_caps_free(buf); return; }

    wasm_module_inst_t inst = wasm_runtime_instantiate(mod, WSTACK, 0, err, sizeof(err));
    if (!inst) { ESP_LOGE(TAG, "[%s] INSTANTIATE FAIL: %s", m->label, err);
                 wasm_runtime_unload(mod); heap_caps_free(buf); return; }

    wasm_exec_env_t env = wasm_runtime_create_exec_env(inst, WSTACK);
    int32_t n = wasm_runtime_get_export_count(mod);
    int funcs = 0, ran = 0;
    ESP_LOGI(TAG, "[%s] LOADED ok — %u bytes, %d exports", m->label, (unsigned)sz, (int)n);

    for (int32_t i = 0; i < n; i++) {
        wasm_export_t ex;
        wasm_runtime_get_export_type(mod, i, &ex);
        if (ex.kind != WASM_IMPORT_EXPORT_KIND_FUNC) continue;
        funcs++;
        uint32_t pc = wasm_func_type_get_param_count(ex.u.func_type);
        uint32_t rc = wasm_func_type_get_result_count(ex.u.func_type);
        if (pc > 8) pc = 8;                       /* safety clamp */
        uint32_t argv[16] = {0};                  /* zero-args; i32 assumed */
        wasm_function_inst_t f = wasm_runtime_lookup_function(inst, ex.name);
        if (f && wasm_runtime_call_wasm(env, f, pc, argv)) {
            ran++;
            if (rc > 0) ESP_LOGI(TAG, "   ✓ %s(%u)=%u", ex.name, (unsigned)pc, (unsigned)argv[0]);
            else        ESP_LOGI(TAG, "   ✓ %s(%u) (void)", ex.name, (unsigned)pc);
        } else {
            ESP_LOGW(TAG, "   ✗ %s trapped: %s", ex.name, wasm_runtime_get_exception(inst));
        }
    }
    ESP_LOGI(TAG, "[%s] RESULT: %d/%d func exports ran on-chip  %s",
             m->label, ran, funcs, (funcs > 0 && ran == funcs) ? "PASS" : (ran ? "PARTIAL" : "FAIL"));

    if (env) wasm_runtime_destroy_exec_env(env);
    wasm_runtime_deinstantiate(inst);
    wasm_runtime_unload(mod);
    heap_caps_free(buf);
}

static void run_smoke(void)
{
    ESP_LOGI(TAG, "================ workshop wasm smoke-test (WAMR / ESP32-S3) ================");
    ESP_LOGI(TAG, "modules to test: %u", (unsigned)N_MODULES);
    RuntimeInitArgs init = {0};
    init.mem_alloc_type = Alloc_With_System_Allocator;
    if (!wasm_runtime_full_init(&init)) { ESP_LOGE(TAG, "WAMR init failed"); return; }
    for (unsigned i = 0; i < N_MODULES; i++) test_one(&TABLE[i]);
    wasm_runtime_destroy();
    ESP_LOGI(TAG, "================ smoke-test complete ================");
}

static void *thr(void *a) { run_smoke(); return NULL; }

void app_main(void)
{
    ESP_LOGI(TAG, "== workshop PR fleet — wasm-on-ESP32-S3 smoke test (WAMR) ==");
    pthread_attr_t attr; pthread_attr_init(&attr); pthread_attr_setstacksize(&attr, 16 * 1024);
    pthread_t t; pthread_create(&t, &attr, thr, NULL); pthread_join(t, NULL);
    ESP_LOGI(TAG, "done — looping");
    while (1) vTaskDelay(pdMS_TO_TICKS(2000));
}
