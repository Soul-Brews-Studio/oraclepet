/*
 * petsim — run a pet app on your Mac with the pet's own runtime and API.
 *
 *   petsim build/hello.wasm             load, on_start, a tap, a fleet beat, 2 s of ticks
 *   petsim --check build/hello.wasm     load + instantiate only (build.sh uses this)
 *
 * The natives print what the pet would do ("show busy", "caption 'hello'").
 * Exit 1 when the pet would refuse the module or the app traps.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wasm_export.h"

static const char *STATES[] = { "idle", "busy", "attention", "sleep" };
static int ms;

static void p(const char *what, const char *s, uint32_t n) { printf("  pet.%-8s '%.*s'\n", what, (int)n, s); }
static void n_show(wasm_exec_env_t e, int32_t st) { printf("  pet.show     %s\n", st >= 0 && st < 4 ? STATES[st] : "?"); }
static void n_react(wasm_exec_env_t e) { printf("  pet.react    (heart)\n"); }
static void n_caption(wasm_exec_env_t e, const char *s, uint32_t n) { p("caption", s, n); }
static void n_status(wasm_exec_env_t e, const char *s, uint32_t n) { p("status", s, n); }
static int32_t n_pack(wasm_exec_env_t e, const char *s, uint32_t n) { p("pack", s, n); return 1; }
static void n_sound(wasm_exec_env_t e, int32_t k) { printf("  pet.sound    %s\n", k == 0 ? "ribbit" : k == 1 ? "blip up" : "blip down"); }
static int32_t n_millis(wasm_exec_env_t e) { return ms; }
static void n_log(wasm_exec_env_t e, const char *s, uint32_t n) { p("log", s, n); }
static void n_tone(wasm_exec_env_t e, int32_t hz, int32_t ms) { printf("  pet.tone     %d Hz, %d ms\n", hz, ms); }
static int32_t n_play(wasm_exec_env_t e, const char *s, uint32_t n) { p("play", s, n); return 1; }
static int32_t n_say(wasm_exec_env_t e, const char *s, uint32_t n) { p("say", s, n); return 1; }

static int draws;   /* canvas calls are counted, not printed (an animation makes hundreds) */
static void n_canvas(wasm_exec_env_t e, int32_t on) { printf("  pet.canvas   %s\n", on ? "on" : "off"); }
static void n_clear(wasm_exec_env_t e, int32_t c) { draws++; }
static void n_rect(wasm_exec_env_t e, int32_t x, int32_t y, int32_t w, int32_t h, int32_t c) { draws++; }
static void n_circle(wasm_exec_env_t e, int32_t x, int32_t y, int32_t r, int32_t c) { draws++; }
static void n_line(wasm_exec_env_t e, int32_t a, int32_t b, int32_t c, int32_t d, int32_t col) { draws++; }
static void n_text(wasm_exec_env_t e, int32_t x, int32_t y, int32_t sc, int32_t c, const char *s, uint32_t n) { draws++; }
static int32_t n_width(wasm_exec_env_t e) { return 320; }
static int32_t n_height(wasm_exec_env_t e) { return 400; }

static NativeSymbol natives[] = {   /* same table as the firmware (main/petapp.c) */
    { "show", n_show, "(i)", NULL }, { "react", n_react, "()", NULL },
    { "caption", n_caption, "(*~)", NULL }, { "status", n_status, "(*~)", NULL },
    { "pack", n_pack, "(*~)i", NULL }, { "sound", n_sound, "(i)", NULL },
    { "millis", n_millis, "()i", NULL }, { "log", n_log, "(*~)", NULL },
    { "tone", n_tone, "(ii)", NULL }, { "play", n_play, "(*~)i", NULL },
    { "say", n_say, "(*~)i", NULL },
    { "canvas", n_canvas, "(i)", NULL }, { "clear", n_clear, "(i)", NULL },
    { "rect", n_rect, "(iiiii)", NULL }, { "circle", n_circle, "(iiii)", NULL },
    { "line", n_line, "(iiiii)", NULL }, { "text", n_text, "(iiii*~)", NULL },
    { "width", n_width, "()i", NULL }, { "height", n_height, "()i", NULL },
};

static int call(wasm_exec_env_t env, wasm_module_inst_t inst, const char *name, uint32_t argc, uint32_t *argv)
{
    wasm_function_inst_t f = wasm_runtime_lookup_function(inst, name);
    if (!f) return 0;
    if (!wasm_runtime_call_wasm(env, f, argc, argv)) {
        fprintf(stderr, "✗ %s trapped: %s\n", name, wasm_runtime_get_exception(inst));
        return -1;
    }
    return 1;
}

int main(int argc, char **argv)
{
    int check = argc > 2 && strcmp(argv[1], "--check") == 0;
    const char *path = argv[argc - 1];
    if (argc < 2) { fprintf(stderr, "usage: petsim [--check] app.wasm\n"); return 2; }
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return 2; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(sz);
    if (fread(buf, 1, sz, f) != (size_t)sz) { fclose(f); return 2; }
    fclose(f);

    RuntimeInitArgs a = { 0 };
    a.mem_alloc_type = Alloc_With_System_Allocator;
    a.native_module_name = "pet";
    a.native_symbols = natives;
    a.n_native_symbols = sizeof natives / sizeof natives[0];
    if (!wasm_runtime_full_init(&a)) { fprintf(stderr, "WAMR init failed\n"); return 2; }

    char err[128];
    wasm_module_t mod = wasm_runtime_load(buf, (uint32_t)sz, err, sizeof err);
    if (!mod) { fprintf(stderr, "✗ the pet would refuse %s: %s\n", path, err); return 1; }
    wasm_module_inst_t inst = wasm_runtime_instantiate(mod, 16 * 1024, 8 * 1024, err, sizeof err);   /* = petapp.c */
    if (!inst) { fprintf(stderr, "✗ instantiate %s: %s\n", path, err); return 1; }
    wasm_exec_env_t env = wasm_runtime_create_exec_env(inst, 16 * 1024);
    if (check) { printf("✓ loads on the pet's WAMR: %s\n", path); return 0; }

    printf("▶ %s (%ld bytes)\n", path, sz);
    uint32_t v[3];
    if (call(env, inst, "_initialize", 0, NULL) < 0 || call(env, inst, "on_start", 0, NULL) < 0) return 1;
    printf("· tap\n");  v[0] = 160; v[1] = 240; if (call(env, inst, "on_tap", 2, v) < 0) return 1;
    printf("· fleet 3 total, 2 running, 1 waiting\n"); v[0] = 3; v[1] = 2; v[2] = 1; if (call(env, inst, "on_fleet", 3, v) < 0) return 1;
    printf("· fleet all answered\n"); v[0] = 3; v[1] = 2; v[2] = 0; if (call(env, inst, "on_fleet", 3, v) < 0) return 1;
    printf("· 2 s of ticks\n");
    for (ms = 100; ms <= 2000; ms += 100) { v[0] = ms; if (call(env, inst, "on_tick", 1, v) < 0) return 1; }
    printf("✓ ok (%d canvas draws)\n", draws);
    return 0;
}
