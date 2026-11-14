/*
 * oracle-face v2 simulator — 10-card brewing dashboard
 *
 * Mirrors the layout in jc3248w535.yaml (commit b29668c) so we can iterate
 * on positions, fonts, and colors without flashing the board every change.
 *
 * Icons in the YAML use Material Icons font (chip, opacity, flame); the
 * sim renders them as small cyan/orange rectangles since we don't ship
 * the MI font subset to the sim yet. Layout dimensions are pixel-accurate
 * with the YAML.
 *
 * Build: see CMakeLists.txt (target: oracle_face_sim_v2)
 * Run:   ./build/oracle_face_sim_v2  (or `make sim-v2` from lab/jc3248w535)
 */

#include "lvgl.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

extern const lv_font_t noto_14;
extern const lv_font_t noto_22;
extern const lv_font_t noto_28;

#define SCREEN_W 320
#define SCREEN_H 480

static lv_obj_t *heartbeat_label;
static lv_obj_t *day_bar;
static int heartbeat_n = 0;

static void heartbeat_cb(lv_timer_t *timer)
{
    (void)timer;
    heartbeat_n++;
    char buf[48];
    snprintf(buf, sizeof(buf), "TENDING THE FLAME [%d]", heartbeat_n);
    lv_label_set_text(heartbeat_label, buf);
    printf("[%d] tending the flame\n", heartbeat_n);
}

static lv_obj_t *make_card(lv_obj_t *parent, int x, int y, int w, int h, uint32_t border_color, int pad)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x0A1419), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(border_color), 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_pad_all(card, pad, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, uint32_t color,
                            const lv_font_t *font, lv_align_t align, int x, int y)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_align(lbl, align, x, y);
    return lbl;
}

/* Small colored rectangle as icon placeholder (sim doesn't ship Material Icons yet). */
static lv_obj_t *make_icon_placeholder(lv_obj_t *parent, lv_align_t align, int x, int y,
                                       int size, uint32_t color)
{
    lv_obj_t *icon = lv_obj_create(parent);
    lv_obj_set_size(icon, size, size);
    lv_obj_align(icon, align, x, y);
    lv_obj_set_style_bg_color(icon, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(icon, 0, 0);
    lv_obj_set_style_radius(icon, 3, 0);
    lv_obj_set_style_pad_all(icon, 0, 0);
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_SCROLLABLE);
    return icon;
}

static void build_ui(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_set_style_border_width(screen, 0, 0);

    /* ── Card 1: Header — Xiaoer Oracle + BREW DAY pill ──────────── */
    lv_obj_t *card1 = make_card(screen, 8, 8, 304, 70, 0x1F3540, 14);
    make_label(card1, "Xiaoer Oracle", 0xFFFFFF, &noto_22, LV_ALIGN_TOP_LEFT, 0, 0);
    make_label(card1, "first body with a screen", 0x888888, &noto_14, LV_ALIGN_TOP_LEFT, 0, 28);

    lv_obj_t *pill = lv_obj_create(card1);
    lv_obj_set_size(pill, 96, 26);
    lv_obj_align(pill, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(pill, lv_color_hex(0x0A1419), 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(pill, 1, 0);
    lv_obj_set_style_border_color(pill, lv_color_hex(0x00FF7F), 0);
    lv_obj_set_style_radius(pill, 13, 0);
    lv_obj_set_style_pad_all(pill, 0, 0);
    lv_obj_clear_flag(pill, LV_OBJ_FLAG_SCROLLABLE);
    make_label(pill, "FERMENTING", 0x00FF7F, &noto_14, LV_ALIGN_CENTER, 0, 0);

    /* ── Card 2: DAY counter + progress bar ──────────────────────── */
    /* Baselines aligned by giving each font its own y to land on baseline 24.
       Initial label text is "?" — placeholder until MQTT pushes a real value.
       The sim has no MQTT client, so it stays as "?" by design — useful for
       layout review of the empty/just-booted state. */
    lv_obj_t *card2 = make_card(screen, 8, 86, 304, 68, 0x1F3540, 14);
    make_label(card2, "DAY", 0x888888, &noto_14, LV_ALIGN_TOP_LEFT,  0, 12);
    make_label(card2, "?",   0x00FF7F, &noto_28, LV_ALIGN_TOP_LEFT, 48,  0);
    make_label(card2, "/ ?", 0x888888, &noto_22, LV_ALIGN_TOP_LEFT, 92,  6);

    day_bar = lv_bar_create(card2);
    lv_obj_set_size(day_bar, 264, 6);
    lv_obj_align(day_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_bar_set_range(day_bar, 0, 24);
    lv_bar_set_value(day_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(day_bar, lv_color_hex(0x1F3540), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(day_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(day_bar, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(day_bar, lv_color_hex(0x00FF7F), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(day_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(day_bar, 3, LV_PART_INDICATOR);

    /* ── Card 3: BREWING batch info ──────────────────────────────── */
    lv_obj_t *card3 = make_card(screen, 8, 162, 304, 90, 0x1F3540, 14);
    make_label(card3, "BREWING", 0x4FB3FF, &noto_14, LV_ALIGN_TOP_LEFT, 0, 0);
    make_label(card3, "Neo Genesis", 0xFFFFFF, &noto_28, LV_ALIGN_TOP_LEFT, 0, 20);
    make_label(card3, "American IPA", 0x4FB3FF, &noto_14, LV_ALIGN_TOP_LEFT, 0, 52);
    make_label(card3, "ESP32", 0x4FB3FF, &noto_14, LV_ALIGN_TOP_RIGHT, 0, 36);
    make_icon_placeholder(card3, LV_ALIGN_TOP_RIGHT, 0, 4, 18, 0x4FB3FF);

    /* ── Cards 4+5: VOLUME + GRAIN side-by-side ──────────────────── */
    lv_obj_t *card4 = make_card(screen, 8, 260, 148, 78, 0x1F3540, 12);
    make_label(card4, "VOLUME", 0x4FB3FF, &noto_14, LV_ALIGN_TOP_LEFT, 0, 0);
    make_label(card4, "530 L", 0xFFFFFF, &noto_22, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    lv_obj_t *card5 = make_card(screen, 164, 260, 148, 78, 0x1F3540, 12);
    make_label(card5, "GRAIN", 0x4FB3FF, &noto_14, LV_ALIGN_TOP_LEFT, 0, 0);
    make_label(card5, "139 kg", 0xFFFFFF, &noto_22, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    /* ── Cards 6-9: OG / FG / ABV / IBU (4-up grid) ──────────────── */
    /* OG + FG masked (?.???) — not yet measured. ABV/IBU are recipe targets. */
    struct { int x; int w; const char *label; const char *value; } gravs[] = {
        {   8, 73, "OG",  "?.???" },
        {  89, 73, "FG",  "?.???" },
        { 170, 73, "ABV", "6.6%"  },
        { 251, 61, "IBU", "66"    },
    };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *c = make_card(screen, gravs[i].x, 346, gravs[i].w, 60, 0x1F3540, 6);
        make_label(c, gravs[i].label, 0x4FB3FF, &noto_14, LV_ALIGN_TOP_MID,    0,  0);
        make_label(c, gravs[i].value, 0x00FF7F, &noto_14, LV_ALIGN_BOTTOM_MID, 0, -4);
    }

    /* ── Card 10: TENDING THE FLAME (orange) ─────────────────────── */
    lv_obj_t *card10 = make_card(screen, 8, 416, 304, 50, 0xFFD27F, 12);
    /* Icon + text centered as a group. align_to keeps text snug to icon's right,
       so when the heartbeat counter grows the pair stays visually balanced. */
    lv_obj_t *flame = make_icon_placeholder(card10, LV_ALIGN_CENTER, -95, 0, 18, 0xFFD27F);
    heartbeat_label = lv_label_create(card10);
    lv_label_set_text(heartbeat_label, "TENDING THE FLAME [0]");
    lv_obj_set_style_text_color(heartbeat_label, lv_color_hex(0xFFD27F), 0);
    lv_obj_set_style_text_font(heartbeat_label, &noto_14, 0);
    lv_obj_align_to(heartbeat_label, flame, LV_ALIGN_OUT_RIGHT_MID, 8, 0);

    lv_timer_create(heartbeat_cb, 2000, NULL);
}

static Uint32 g_last_tick;

static void tick(void *arg)
{
    (void)arg;
    Uint32 now = SDL_GetTicks();
    lv_tick_inc(now - g_last_tick);
    g_last_tick = now;
    lv_timer_handler();
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    setvbuf(stdout, NULL, _IOLBF, 0);
    lv_init();
    lv_display_t *disp = lv_sdl_window_create(SCREEN_W, SCREEN_H);
    if (!disp) {
        fprintf(stderr, "lv_sdl_window_create failed\n");
        return 1;
    }
    lv_sdl_window_set_title(disp, "oracle-face v2 (sim)");
    lv_sdl_mouse_create();

    build_ui();
    printf("oracle-face v2 sim — %dx%d (icons rendered as solid rectangles)\n", SCREEN_W, SCREEN_H);

    g_last_tick = SDL_GetTicks();

#ifdef __EMSCRIPTEN__
    /* In the browser, hand the main loop to requestAnimationFrame so the
       page stays responsive. fps=0 means "use rAF natural cadence". */
    emscripten_set_main_loop_arg(tick, NULL, 0, 1);
#else
    while (1) {
        tick(NULL);
        SDL_Delay(16);
    }
#endif
    return 0;
}
