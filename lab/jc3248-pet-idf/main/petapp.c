/*
 * petapp.c — jc3248-pet-idf STEP 12: the WAMR host for pet apps. API in petapp.h.
 *
 * Two threads, two queues (lessons from lab/gif-wamr, which got WAMR 2.4 running
 * on IDF v6 first):
 *   - WAMR runs on its OWN pthread. Its os_self_thread() calls pthread_self(),
 *     which asserts on a plain FreeRTOS task such as app_main's.
 *   - The GIF engine and the HUD are main-thread only. So the main loop posts
 *     events (run, tick, tap, fleet) to the app thread, and the natives post
 *     commands (show, caption, pack, ...) back; petapp_poll() applies them.
 *
 * Safety: each call into the app runs under a 2 s watchdog (overrun: restart and
 * the next boot skips the app); a trap stops the app; and after a panic or
 * watchdog reset the saved app is not started, so a bad app cannot boot-loop.
 */
#include "petapp.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"

#include "wasm_export.h"

#include "audio.h"
#include "fs.h"
#include "gif.h"
#include "hud.h"
#include "hud_gfx.h"
#include "ota.h"
#include "ble.h"

static const char *TAG = "petapp";

#define STACK_SIZE   (16 * 1024)    /* wasm operand/call stack per app */
#define HEAP_SIZE    (8 * 1024)     /* app heap (libc builtin malloc) */
#define THREAD_STACK (24 * 1024)    /* the pthread WAMR runs on */
#define WATCHDOG_US  (2 * 1000 * 1000)
#define MAX_WASM     (256 * 1024)

/* main -> app thread */
typedef enum { EV_RUN, EV_TICK, EV_TAP, EV_FLEET } ev_kind_t;
typedef struct { ev_kind_t kind; int a, b, c; char name[24]; } ev_t;
/* app thread -> main */
typedef enum { CMD_SHOW, CMD_REACT, CMD_CAPTION, CMD_STATUS, CMD_PACK, CMD_SOUND, CMD_STOPPED } cmd_kind_t;
typedef struct { cmd_kind_t kind; int n; char text[64]; } cmd_t;

static QueueHandle_t s_ev, s_cmd;
static esp_timer_handle_t s_watchdog;

/* app-thread state */
static uint8_t           *s_buf;
static wasm_module_t      s_module;
static wasm_module_inst_t s_inst;
static wasm_exec_env_t    s_env;
static wasm_function_inst_t s_on_tick, s_on_tap, s_on_fleet;

/* shared, written by the app thread, read by main */
static char          s_name[24];
static char          s_err[96];
static volatile bool s_owns_state, s_owns_caption, s_has_tick, s_has_tap;

/* ---- NVS ---- */
static void nvs_put(const char *key, const char *val)
{
    nvs_handle_t h;
    if (nvs_open("pet", NVS_READWRITE, &h) != ESP_OK) return;
    if (val) nvs_set_str(h, key, val); else nvs_erase_key(h, key);
    nvs_commit(h);
    nvs_close(h);
}

static bool nvs_get(const char *key, char *out, size_t n)
{
    nvs_handle_t h;
    out[0] = 0;
    if (nvs_open("pet", NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = n;
    bool ok = nvs_get_str(h, key, out, &len) == ESP_OK;
    nvs_close(h);
    return ok && out[0];
}

/* Fires on the esp_timer task while the app thread spins inside wasm. */
static void watchdog_fired(void *arg)
{
    esp_rom_printf("petapp: '%s' did not return in 2 s; restarting without it\n", s_name);
    nvs_put("app_bad", s_name);
    esp_restart();
}

/* ---- natives (module "pet"), on the app thread: post to main ---- */
static void post(cmd_kind_t k, int n, const char *s, uint32_t len)
{
    cmd_t c = { .kind = k, .n = n };
    if (s) {
        if (len >= sizeof c.text) len = sizeof c.text - 1;
        memcpy(c.text, s, len);
        c.text[len] = 0;
    }
    xQueueSend(s_cmd, &c, 0);          /* a flooding app loses commands, not the pet */
}

static void n_show(wasm_exec_env_t env, int32_t state) { post(CMD_SHOW, state, NULL, 0); }
static void n_react(wasm_exec_env_t env) { post(CMD_REACT, 0, NULL, 0); }
static void n_caption(wasm_exec_env_t env, const char *s, uint32_t len) { s_owns_caption = true; post(CMD_CAPTION, 0, s, len < 24 ? len : 23); }
static void n_status(wasm_exec_env_t env, const char *s, uint32_t len) { post(CMD_STATUS, 0, s, len); }
static void n_sound(wasm_exec_env_t env, int32_t kind) { post(CMD_SOUND, kind, NULL, 0); }
static int32_t n_millis(wasm_exec_env_t env) { return (int32_t)(esp_timer_get_time() / 1000); }
/* say(text): the pet has no speech synthesis, so it asks the linked OraclePet, which
 * speaks it with macOS speech, sends the clip back (sounds/say.ima) and plays it.
 * {"evt":"say","text":"..."} on NUS TX; JSON-escaped, at most 120 bytes of text. */
static int32_t n_say(wasm_exec_env_t env, const char *s, uint32_t len)
{
    char j[180];
    int k = snprintf(j, sizeof j, "{\"evt\":\"say\",\"text\":\"");
    for (uint32_t i = 0; i < len && i < 120 && k < (int)sizeof j - 4; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') { j[k++] = '\\'; j[k++] = (char)c; }
        else if (c >= 0x20) j[k++] = (char)c;              /* UTF-8 bytes pass through */
    }
    k += snprintf(j + k, sizeof j - k, "\"}");
    return bleNotifyLine(j) ? 1 : 0;
}

/* Sound natives go straight to the audio task's queue (thread-safe, never block). */
static void n_tone(wasm_exec_env_t env, int32_t hz, int32_t ms) { audio_tone(hz, ms); }
static int32_t n_play(wasm_exec_env_t env, const char *s, uint32_t len)
{
    char name[24], path[64];
    struct stat st;
    if (len == 0 || len >= sizeof name) return 0;
    memcpy(name, s, len);
    name[len] = 0;
    snprintf(path, sizeof path, "%s/sounds/%s.ima", FS_MOUNT_POINT, name);
    if (stat(path, &st) != 0) return 0;
    return audio_play(name) ? 1 : 0;
}

/* 1 when the pack exists on the pet (the switch itself happens on the main loop). */
static int32_t n_pack(wasm_exec_env_t env, const char *s, uint32_t len)
{
    char name[24], path[64];
    struct stat st;
    if (len == 0 || len >= sizeof name) return 0;
    memcpy(name, s, len);
    name[len] = 0;
    snprintf(path, sizeof path, "%s/characters/%s/manifest.json", FS_MOUNT_POINT, name);
    if (stat(path, &st) != 0) return 0;
    post(CMD_PACK, 0, name, len);
    return 1;
}

static void n_log(wasm_exec_env_t env, const char *s, uint32_t len)
{
    char b[128];
    if (len >= sizeof b) len = sizeof b - 1;
    memcpy(b, s, len);
    b[len] = 0;
    ESP_LOGI(TAG, "[%s] %s", s_name, b);
}

/* ---- canvas mode: the app draws its own screen ----
 * pet_canvas(1) pauses the cat and hands the app the area above the HUD (DISP_W x HUD_Y0).
 * The app thread draws straight into s_canvas (PSRAM, big-endian pixels like the framebuffer,
 * through hud_gfx) and holds s_canvas_mx for the whole callback, so the main loop only copies
 * finished frames: petapp_canvas_present() after each callback that drew. */
#define CV_W DISP_W
#define CV_H HUD_Y0
static uint16_t *s_canvas;
static SemaphoreHandle_t s_canvas_mx;
static volatile bool s_canvas_on, s_canvas_dirty, s_canvas_left;

static void n_canvas(wasm_exec_env_t env, int32_t on)
{
    if (on && !s_canvas) s_canvas = heap_caps_malloc((size_t)CV_W * CV_H * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (on && s_canvas) {
        if (!s_canvas_on) memset(s_canvas, 0, (size_t)CV_W * CV_H * 2);
        s_canvas_on = s_canvas_dirty = true;
        s_owns_state = s_owns_caption = true;      /* the heartbeat no longer drives the cat */
    } else if (!on && s_canvas_on) {
        s_canvas_on = false;
        s_canvas_left = true;
    }
}
static void n_clear(wasm_exec_env_t env, int32_t c)
{
    if (!s_canvas_on) return;
    gfx_fill_rect(s_canvas, CV_W, CV_H, 0, 0, CV_W, CV_H, (uint16_t)c);
    s_canvas_dirty = true;
}
static void n_rect(wasm_exec_env_t env, int32_t x, int32_t y, int32_t w, int32_t h, int32_t c)
{
    if (!s_canvas_on) return;
    gfx_fill_rect(s_canvas, CV_W, CV_H, x, y, w, h, (uint16_t)c);
    s_canvas_dirty = true;
}
static void n_circle(wasm_exec_env_t env, int32_t x, int32_t y, int32_t r, int32_t c)
{
    if (!s_canvas_on || r < 0 || r > 1000) return;
    gfx_fill_circle(s_canvas, CV_W, CV_H, x, y, r, (uint16_t)c);
    s_canvas_dirty = true;
}
static void n_line(wasm_exec_env_t env, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t c)
{
    if (!s_canvas_on) return;
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (int n = 0; n < 4000; n++) {                 /* bounded: a line is at most ~600 px here */
        gfx_pixel(s_canvas, CV_W, CV_H, x0, y0, (uint16_t)c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
    s_canvas_dirty = true;
}
static void n_text(wasm_exec_env_t env, int32_t x, int32_t y, int32_t scale, int32_t c, const char *s, uint32_t len)
{
    char b[64];
    if (!s_canvas_on) return;
    if (len >= sizeof b) len = sizeof b - 1;
    memcpy(b, s, len);
    b[len] = 0;
    gfx_text(s_canvas, CV_W, CV_H, x, y, b, scale < 1 ? 1 : scale > 8 ? 8 : scale, (uint16_t)c, GFX_TRANSPARENT);
    s_canvas_dirty = true;
}
static int32_t n_width(wasm_exec_env_t env) { return CV_W; }
static int32_t n_height(wasm_exec_env_t env) { return CV_H; }

/* "(*~)" = pointer + length, checked by WAMR against the app's memory. */
static NativeSymbol s_natives[] = {
    { "show",    n_show,    "(i)",   NULL },
    { "react",   n_react,   "()",    NULL },
    { "caption", n_caption, "(*~)",  NULL },
    { "status",  n_status,  "(*~)",  NULL },
    { "pack",    n_pack,    "(*~)i", NULL },
    { "sound",   n_sound,   "(i)",   NULL },
    { "millis",  n_millis,  "()i",   NULL },
    { "log",     n_log,     "(*~)",  NULL },
    { "tone",    n_tone,    "(ii)",  NULL },
    { "play",    n_play,    "(*~)i", NULL },
    { "say",     n_say,     "(*~)i", NULL },
    { "canvas",  n_canvas,  "(i)",   NULL },
    { "clear",   n_clear,   "(i)",   NULL },
    { "rect",    n_rect,    "(iiiii)", NULL },
    { "circle",  n_circle,  "(iiii)", NULL },
    { "line",    n_line,    "(iiiii)", NULL },
    { "text",    n_text,    "(iiii*~)", NULL },
    { "width",   n_width,   "()i",   NULL },
    { "height",  n_height,  "()i",   NULL },
};

/* ---- app lifecycle (app thread) ---- */
static void unload(void)
{
    bool had = s_inst != NULL;
    s_has_tick = s_has_tap = false;
    s_owns_state = s_owns_caption = false;
    if (s_canvas_on) { s_canvas_on = false; s_canvas_left = true; }
    if (s_env)    { wasm_runtime_destroy_exec_env(s_env); s_env = NULL; }
    if (s_inst)   { wasm_runtime_deinstantiate(s_inst); s_inst = NULL; }
    if (s_module) { wasm_runtime_unload(s_module); s_module = NULL; }
    free(s_buf);
    s_buf = NULL;
    s_on_tick = s_on_tap = s_on_fleet = NULL;
    s_name[0] = 0;
    if (had) post(CMD_STOPPED, 0, NULL, 0);
}

static bool call(wasm_function_inst_t fn, uint32_t argc, uint32_t *argv)
{
    if (!fn || !s_env) return false;
    xSemaphoreTake(s_canvas_mx, portMAX_DELAY);     /* the main loop copies only finished canvas frames */
    esp_timer_start_once(s_watchdog, WATCHDOG_US);
    bool ok = wasm_runtime_call_wasm(s_env, fn, argc, argv);
    esp_timer_stop(s_watchdog);
    xSemaphoreGive(s_canvas_mx);
    if (!ok) {
        snprintf(s_err, sizeof s_err, "%.23s trapped: %.60s", s_name, wasm_runtime_get_exception(s_inst));
        ESP_LOGE(TAG, "%s; app stopped", s_err);
        unload();
    }
    return ok;
}

static bool load(const char *name)
{
    char path[64];
    snprintf(path, sizeof path, "%s/apps/%.23s.wasm", FS_MOUNT_POINT, name);
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(s_err, sizeof s_err, "no app %.23s on the pet", name); return false; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 8 || sz > MAX_WASM) { fclose(f); snprintf(s_err, sizeof s_err, "bad size %ld", sz); return false; }
    /* WAMR rewrites the module bytes while loading: they must be in writable RAM. */
    s_buf = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_buf || fread(s_buf, 1, sz, f) != (size_t)sz) {
        fclose(f); free(s_buf); s_buf = NULL;
        snprintf(s_err, sizeof s_err, "read failed");
        return false;
    }
    fclose(f);

    char eb[96];
    s_module = wasm_runtime_load(s_buf, (uint32_t)sz, eb, sizeof eb);
    if (!s_module) { snprintf(s_err, sizeof s_err, "load: %.60s", eb); unload(); return false; }
    s_inst = wasm_runtime_instantiate(s_module, STACK_SIZE, HEAP_SIZE, eb, sizeof eb);
    if (!s_inst) { snprintf(s_err, sizeof s_err, "instantiate: %.60s", eb); unload(); return false; }
    s_env = wasm_runtime_create_exec_env(s_inst, STACK_SIZE);
    if (!s_env) { snprintf(s_err, sizeof s_err, "exec env failed"); unload(); return false; }

    snprintf(s_name, sizeof s_name, "%s", name);
    s_on_tick  = wasm_runtime_lookup_function(s_inst, "on_tick");
    s_on_tap   = wasm_runtime_lookup_function(s_inst, "on_tap");
    s_on_fleet = wasm_runtime_lookup_function(s_inst, "on_fleet");
    s_has_tick = s_on_tick != NULL;
    s_has_tap  = s_on_tap != NULL;
    s_owns_state = s_on_fleet != NULL;
    s_err[0] = 0;
    ESP_LOGI(TAG, "app '%s' loaded (%ld bytes; tick=%d tap=%d fleet=%d)", name, sz,
             s_has_tick, s_has_tap, s_owns_state);
    wasm_function_inst_t init = wasm_runtime_lookup_function(s_inst, "_initialize");   /* reactor modules */
    if (init && !call(init, 0, NULL)) return false;
    wasm_function_inst_t start = wasm_runtime_lookup_function(s_inst, "on_start");
    if (start && !call(start, 0, NULL)) return false;
    return true;
}

static void run(const char *name, bool from_host)
{
    unload();
    bool ok = true;
    if (name[0]) {
        ok = load(name);
        if (ok) { nvs_put("app", name); nvs_put("app_bad", NULL); }
        else {
            ESP_LOGE(TAG, "run '%s' failed: %s", name, s_err);
            /* WAMR 2.4's fast-interp loader can corrupt the heap on a rejected module
             * (seen: br_table type mismatch, then a TLSF assert on the next load). If the
             * heap is damaged, tell the host, forget the app and restart cleanly. */
            if (!heap_caps_check_integrity_all(true)) {
                ESP_LOGE(TAG, "heap damaged by the failed load; restarting");
                strlcat(s_err, " (pet restarted)", sizeof s_err);
                ota_app_result(name, false, s_err);
                nvs_put("app", NULL);
                vTaskDelay(pdMS_TO_TICKS(500));
                esp_restart();
            }
        }
    } else {
        nvs_put("app", NULL);
        s_err[0] = 0;
        ESP_LOGI(TAG, "app stopped");
    }
    if (from_host) ota_app_result(name, ok, s_err);
}

static void *app_thread(void *arg)
{
    RuntimeInitArgs a;
    memset(&a, 0, sizeof a);
    a.mem_alloc_type = Alloc_With_System_Allocator;   /* malloc: big blocks land in PSRAM */
    a.native_module_name = "pet";
    a.native_symbols = s_natives;
    a.n_native_symbols = sizeof s_natives / sizeof s_natives[0];
    if (!wasm_runtime_full_init(&a)) {
        snprintf(s_err, sizeof s_err, "WAMR init failed");
        ESP_LOGE(TAG, "%s", s_err);
        return NULL;
    }
    ESP_LOGI(TAG, "WAMR ready on its own pthread");
    ev_t e;
    for (;;) {
        if (xQueueReceive(s_ev, &e, portMAX_DELAY) != pdTRUE) continue;
        uint32_t argv[3];
        switch (e.kind) {
        case EV_RUN:   run(e.name, e.a); break;
        case EV_TICK:  argv[0] = (uint32_t)e.a; call(s_on_tick, 1, argv); break;
        case EV_TAP:   argv[0] = (uint32_t)e.a; argv[1] = (uint32_t)e.b; call(s_on_tap, 2, argv); break;
        case EV_FLEET: argv[0] = (uint32_t)e.a; argv[1] = (uint32_t)e.b; argv[2] = (uint32_t)e.c; call(s_on_fleet, 3, argv); break;
        }
    }
    return NULL;
}

/* apps/autorun: one line, the app name (a-z 0-9 - _, max 23), written by a flasher bundle. */
static bool read_autorun(char *out, size_t n)
{
    char path[64], line[32];
    snprintf(path, sizeof path, "%s/apps/autorun", FS_MOUNT_POINT);
    FILE *f = fopen(path, "r");
    if (!f) return false;
    bool ok = fgets(line, sizeof line, f) != NULL;
    fclose(f);
    if (!ok) return false;
    size_t len = strcspn(line, "\r\n \t");
    line[len] = 0;
    if (len == 0 || len > 23) return false;
    for (size_t i = 0; i < len; i++) {
        char c = line[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
    }
    snprintf(out, n, "%s", line);
    return true;
}

/* ---- main-thread API ---- */
void petapp_boot(void)
{
    s_ev  = xQueueCreate(8, sizeof(ev_t));
    s_cmd = xQueueCreate(16, sizeof(cmd_t));
    s_canvas_mx = xSemaphoreCreateMutex();
    esp_timer_create_args_t t = { .callback = watchdog_fired, .name = "petapp_wd" };
    esp_timer_create(&t, &s_watchdog);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, THREAD_STACK);
    pthread_t th;
    if (pthread_create(&th, &attr, app_thread, NULL) != 0) {
        snprintf(s_err, sizeof s_err, "no app thread");
        ESP_LOGE(TAG, "%s", s_err);
        return;
    }

    char name[24], bad[24];
    if (!nvs_get("app", name, sizeof name)) {
        /* A fresh install: the app a flasher bundle baked into storage (apps/autorun holds its
         * name; lab/pet-studio). Once only, so stopping it later sticks; after it starts, run()
         * saves it like any app and the crash guard below covers it from the next boot on. */
        char done[4];
        if (nvs_get("autorun_done", done, sizeof done) || !read_autorun(name, sizeof name)) return;
        nvs_put("autorun_done", "1");
        ESP_LOGI(TAG, "first boot: starting the baked-in app '%s'", name);
    }
    esp_reset_reason_t r = esp_reset_reason();
    if (r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT ||
        (nvs_get("app_bad", bad, sizeof bad) && strcmp(bad, name) == 0)) {
        ESP_LOGW(TAG, "not starting '%s': the last reset was a crash or it overran", name);
        snprintf(s_err, sizeof s_err, "%.23s not started: the pet crashed or it overran last time", name);
        nvs_put("app", NULL);
        nvs_put("app_bad", NULL);
        return;
    }
    ev_t e = { .kind = EV_RUN, .a = 0 };
    snprintf(e.name, sizeof e.name, "%s", name);
    xQueueSend(s_ev, &e, 0);
}

bool petapp_run(const char *name)
{
    if (!s_ev) return false;
    ev_t e = { .kind = EV_RUN, .a = 1 };
    snprintf(e.name, sizeof e.name, "%s", name ? name : "");
    return xQueueSend(s_ev, &e, pdMS_TO_TICKS(100)) == pdTRUE;
}

void petapp_tick(void)
{
    static int64_t last;
    if (!s_ev || !s_has_tick) return;
    int64_t now = esp_timer_get_time();
    if (now - last < 100000) return;
    last = now;
    ev_t e = { .kind = EV_TICK, .a = (int)(now / 1000) };
    xQueueSend(s_ev, &e, 0);           /* a slow app skips ticks */
}

bool petapp_tap(int x, int y)
{
    if (!s_ev || !s_has_tap) return false;
    ev_t e = { .kind = EV_TAP, .a = x, .b = y };
    return xQueueSend(s_ev, &e, 0) == pdTRUE;
}

bool petapp_fleet(int total, int running, int waiting)
{
    if (!s_ev || !s_owns_state) return false;
    ev_t e = { .kind = EV_FLEET, .a = total, .b = running, .c = waiting };
    return xQueueSend(s_ev, &e, 0) == pdTRUE;
}

void petapp_poll(void)
{
    cmd_t c;
    while (s_cmd && xQueueReceive(s_cmd, &c, 0) == pdTRUE) {
        switch (c.kind) {
        case CMD_SHOW:    gif_set_activity(c.n); break;
        case CMD_REACT:   gif_on_touch(); break;
        case CMD_CAPTION: gif_set_who(c.text); break;
        case CMD_STATUS:  hud_set_status(c.text); break;
        case CMD_PACK:    if (gif_select_pack(c.text)) hud_set_name(gif_pack_name()); break;
        case CMD_SOUND:   if (c.n == 0) audio_ribbit(); else audio_blip(c.n == 1); break;
        case CMD_STOPPED: gif_set_who(""); hud_set_status("app stopped"); break;
        }
    }
}

bool petapp_canvas_active(void) { return s_canvas_on; }

bool petapp_canvas_present(uint16_t *fb)
{
    if (!s_canvas_on || !s_canvas_dirty || !fb) return false;
    if (xSemaphoreTake(s_canvas_mx, 0) != pdTRUE) return false;   /* mid-callback: next pass */
    memcpy(fb, s_canvas, (size_t)CV_W * CV_H * 2);
    s_canvas_dirty = false;
    xSemaphoreGive(s_canvas_mx);
    return true;
}

bool petapp_canvas_left(void)
{
    bool l = s_canvas_left;
    s_canvas_left = false;
    return l;
}

bool petapp_owns_state(void)   { return s_owns_state; }
bool petapp_owns_caption(void) { return s_owns_caption; }
const char *petapp_name(void)  { return s_name; }
const char *petapp_error(void) { return s_err; }
