/*
 * jc3248-pet — NATIVE desktop simulator (pet_sim)
 *
 * Previews the bufo desk-pet without flashing hardware. It runs the SAME state
 * machine as the firmware (src/pet.cpp) and loads the SAME character pack
 * (data/characters/bufo/ GIFs) via the LVGL gif decoder + STDIO filesystem.
 *
 * Layout (mirrors pet.cpp):
 *   - A 320x480 "device panel" (black) with the pet animated in the top area
 *     and an 80px HUD strip pinned to the bottom of the panel.
 *   - Below the panel, a 140px control strip of LVGL buttons (so lv_sdl_mouse
 *     handles clicks) that fake Claude activity. SDL window is 320x620.
 *
 * State machine — faithful to pet.cpp computeDesired()/tick_frame():
 *   if !linked OR (now - lastSnap > 30s)  -> idle
 *   else if waiting > 0                   -> attention
 *   else if running > 0                   -> busy
 *   else if total == 0                    -> sleep
 *   else                                  -> idle
 *   idle ROTATES idle_0..idle_8 every ~5s. A tap overrides with "heart" for
 *   4000 ms, then returns to computeDesired().
 *
 * Unlike the firmware (which scans /characters and parses manifest.json), this
 * sim HARDCODES the bufo state->gif mapping and palette below — no JSON parser
 * needed on the desktop. The firmware reads the manifest; the sim mirrors it.
 *
 * GIF base dir: argv[1], default "../../data/characters/bufo" (relative to the
 * binary's cwd, i.e. run from sim/build/). Full paths are "S:" + base + "/" + fn.
 *
 * GIF rendering (the firmware decoder, not lv_gif): LVGL's bundled gifdec
 * overflows/segfaults on bufo's GIFs. So we mirror the device-proven path from
 * src/pet.cpp — decode with AnimatedGIF (bitbank2), 3x nearest-neighbor upscale,
 * transparent index -> bg, centered via gifPlace() — and blit RGB565 straight
 * into an lv_canvas backing buffer. gifplayer.{h,cpp} is the C/C++ bridge; this
 * file stays pure C and never includes a C++ header. The canvas occupies the
 * 320x400 area above the 80px HUD, so centering in the canvas == centering above
 * the HUD (matches gifPlace() in pet.cpp).
 *
 * Build: see CMakeLists.txt (target: pet_sim)
 * Run:   ./build/pet_sim   (or  ./build/pet_sim /abs/path/to/characters/bufo)
 */

#include "lvgl.h"
#include "gifplayer.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

/* ── Panel / window geometry (mirrors PET_W/PET_H/PET_HUD_H in pet.cpp) ── */
#define PANEL_W   320
#define PANEL_H   480
#define HUD_H      80               /* bottom strip of the panel */
#define CTRL_H    140               /* button strip below the panel */
#define WIN_W     PANEL_W
#define WIN_H    (PANEL_H + CTRL_H)  /* 620 */

/* Pet canvas = the usable area above the HUD (mirrors PET_W x (PET_H-PET_HUD_H)
 * in pet.cpp). The AnimatedGIF decoder upscales + centers art into this buffer. */
#define CANVAS_W  PANEL_W            /* 320 */
#define CANVAS_H (PANEL_H - HUD_H)   /* 400 */
#define GIF_SCALE 3                  /* 3x nearest-neighbor upscale (matches pet.cpp) */
#define BG565     0x0000             /* bufo bg black, as RGB565 */

#define REACT_MS      4000U         /* tap -> heart window           */
#define STALE_MS     30000U         /* link goes quiet -> idle       */
#define IDLE_DWELL_MS 5000U         /* time per idle gif before rotate */

/* ── Hardcoded bufo pack (state -> gif). Mirrors data/characters/bufo/
   manifest.json. The firmware parses the JSON; the sim hardcodes it. ── */
static const char *IDLE_GIFS[] = {
    "idle_0.gif", "idle_1.gif", "idle_2.gif", "idle_3.gif", "idle_4.gif",
    "idle_5.gif", "idle_6.gif", "idle_7.gif", "idle_8.gif",
};
#define IDLE_COUNT ((int)(sizeof(IDLE_GIFS) / sizeof(IDLE_GIFS[0])))
static const char *BUSY_GIF  = "busy.gif";
static const char *ATTN_GIF  = "attention.gif";
static const char *SLEEP_GIF = "sleep.gif";
static const char *HEART_GIF = "heart.gif";          /* tap reaction */
#define PACK_NAME "bufo"

/* bufo palette (manifest colors{}): bg black, text white, textDim grey. */
#define COL_BG       0x000000
#define COL_TEXT     0xFFFFFF
#define COL_TEXTDIM  0x808080
#define COL_DOT_LINK 0x33D17A      /* green when linked            */
#define COL_DOT_OFF  0x5A6072      /* grey when not linked         */

/* ── Pet states (mirror enum PetState in pet.cpp) ── */
typedef enum { ST_IDLE, ST_BUSY, ST_ATTENTION, ST_SLEEP } pet_state_t;

/* ── Activity snapshot {total,running,waiting,msg,tokens} + link state ── */
typedef struct {
    int  total, running, waiting;
    long tokens;
    char msg[64];
} snapshot_t;

#ifndef PET_SIM_DEFAULT_PACK
#define PET_SIM_DEFAULT_PACK "../../data/characters/bufo"
#endif
static char        g_base[512] = PET_SIM_DEFAULT_PACK;  /* argv[1] override */
static snapshot_t  g_snap      = { 0, 0, 0, 0, "" };
static bool        g_linked    = false;
static uint32_t    g_lastSnap  = 0;          /* ms of last snapshot/connect */
static uint32_t    g_bootMs    = 0;          /* for the "not linked" uptime */

static pet_state_t g_cur       = ST_IDLE;
static int         g_idleRot   = 0;
static uint32_t    g_idleNextAt = 0;         /* when to rotate the idle gif */
static bool        g_reacting  = false;
static uint32_t    g_reactUntil = 0;

/* GIF frame timing — the AnimatedGIF decoder drives one frame per scheduled
 * tick (unlike lv_gif, which animated itself). g_frameNextAt is when the next
 * frame is due; gp_play_frame() returns the delay to reschedule by. */
static uint32_t    g_frameNextAt = 0;

/* ── LVGL objects ── */
static lv_obj_t *g_pet;          /* lv_canvas (AnimatedGIF blits here) */
static lv_obj_t *g_hudName;      /* large label, "bufo" */
static lv_obj_t *g_hudStatus;    /* status line */
static lv_obj_t *g_dot;          /* connection dot */

/* lv_canvas backing store for the pet (RGB565, CANVAS_W x CANVAS_H). The
 * gifplayer decodes + upscales + centers frames straight into this buffer. */
static uint16_t  PET_CANVAS[CANVAS_W * CANVAS_H];

static uint32_t now_ms(void) { return (uint32_t)SDL_GetTicks(); }

/* Build a plain "<base>/<file>" path into out. gifplayer uses stdio (fopen),
 * NOT LVGL's "S:" drive-letter filesystem, so no prefix here. */
static void gif_path(char *out, size_t n, const char *file) {
    snprintf(out, n, "%s/%s", g_base, file);
}

/* Pick the gif filename for a state (idle uses the current rotation slot). */
static const char *gif_for_state(pet_state_t s) {
    switch (s) {
        case ST_IDLE:      return IDLE_GIFS[g_idleRot % IDLE_COUNT];
        case ST_BUSY:      return BUSY_GIF;
        case ST_ATTENTION: return ATTN_GIF;
        case ST_SLEEP:     return SLEEP_GIF;
    }
    return IDLE_GIFS[0];
}

/* Draw the current frame of the open GIF into the canvas and invalidate it.
   Returns the frame delay (ms) to reschedule by; on end-of-animation rewinds and
   replays so single-gif states loop (mirrors gif.reset() in pet.cpp). */
static uint32_t render_frame(void) {
    int delay = gp_play_frame(PET_CANVAS, CANVAS_W, CANVAS_H, GIF_SCALE, BG565);
    if (delay == 0) {                 /* end of animation -> loop */
        gp_rewind();
        delay = gp_play_frame(PET_CANVAS, CANVAS_W, CANVAS_H, GIF_SCALE, BG565);
    }
    if (g_pet) lv_obj_invalidate(g_pet);
    return delay > 0 ? (uint32_t)delay : 100U;
}

/* Open a state's gif via the AnimatedGIF player and draw frame 0. Re-opening the
   same gif restarts it, which is what we want when rotating idle_N. */
static void show_gif(const char *file) {
    char path[600];
    gif_path(path, sizeof(path), file);
    if (gp_open(path, NULL, NULL) != 0) {
        fprintf(stderr, "[pet] gp_open failed: %s\n", path);
        return;
    }
    g_frameNextAt = now_ms() + render_frame();   /* draw frame 0, arm timing */
}

/* Mirror of pet.cpp computeDesired(). */
static pet_state_t compute_desired(void) {
    if (!g_linked)                          return ST_IDLE;
    if (now_ms() - g_lastSnap > STALE_MS)   return ST_IDLE;  /* link went quiet */
    if (g_snap.waiting > 0)                 return ST_ATTENTION;
    if (g_snap.running > 0)                 return ST_BUSY;
    if (g_snap.total == 0)                  return ST_SLEEP;
    return ST_IDLE;
}

static const char *state_name(pet_state_t s) {
    switch (s) {
        case ST_IDLE:      return "idle";
        case ST_BUSY:      return "busy";
        case ST_ATTENTION: return "attention";
        case ST_SLEEP:     return "sleep";
    }
    return "idle";
}

/* Switch the displayed animation to a state (idle resets/uses its rotation). */
static void enter_state(pet_state_t s) {
    g_cur = s;
    if (s == ST_IDLE) g_idleNextAt = now_ms() + IDLE_DWELL_MS;
    show_gif(gif_for_state(s));
    printf("[pet] state -> %s%s\n", state_name(s),
           s == ST_IDLE ? "" : "");
}

/* Tap reaction — overrides any state with the heart gif for REACT_MS. Re-tap
   extends the window (mirrors pet_on_touch()). */
static void tap_heart(void) {
    g_reactUntil = now_ms() + REACT_MS;
    if (g_reacting) return;          /* already reacting — just extended timer */
    g_reacting = true;
    show_gif(HEART_GIF);
    printf("[pet] tap -> heart\n");
}

/* Update HUD name / status / dot each tick (mirror drawHUD()). */
static void draw_hud(void) {
    lv_label_set_text(g_hudName, PACK_NAME);

    /* Connection dot: green when linked, grey otherwise. */
    lv_obj_set_style_bg_color(g_dot,
        lv_color_hex(g_linked ? COL_DOT_LINK : COL_DOT_OFF), 0);

    char buf[80];
    uint32_t now = now_ms();
    if (g_linked) {
        if (now - g_lastSnap > STALE_MS) {
            snprintf(buf, sizeof(buf), "BLE link stale");
        } else if (g_snap.msg[0]) {
            snprintf(buf, sizeof(buf), "%.60s", g_snap.msg);
        } else {
            snprintf(buf, sizeof(buf), "run %d  wait %d  %ldt",
                     g_snap.running, g_snap.waiting, g_snap.tokens);
        }
    } else {
        uint32_t up = (now - g_bootMs) / 1000U;
        snprintf(buf, sizeof(buf), "%s  sim  %lu:%02lu",
                 g_reacting ? "heart" : "idle",
                 (unsigned long)(up / 60U), (unsigned long)(up % 60U));
    }
    lv_label_set_text(g_hudStatus, buf);
}

/* State-machine timer (mirror tick_frame()): decides WHICH gif shows (state
   changes, idle rotation, heart expiry), advances the CURRENT gif's frames via
   the AnimatedGIF player when each frame's delay elapses, then refreshes the HUD.
   Unlike lv_gif (which self-animated), we now pump every frame ourselves. */
static void pet_timer_cb(lv_timer_t *t) {
    (void)t;
    uint32_t now = now_ms();

    if (g_reacting && now >= g_reactUntil) g_reacting = false;

    if (g_reacting) {
        /* heart overrides; show_gif already set it in tap_heart(). */
    } else {
        pet_state_t want = compute_desired();
        if (want != g_cur) {
            enter_state(want);                 /* show_gif() draws frame 0 + arms timing */
        } else if (g_cur == ST_IDLE && IDLE_COUNT > 1 && now >= g_idleNextAt) {
            g_idleRot = (g_idleRot + 1) % IDLE_COUNT;
            enter_state(ST_IDLE);              /* rotate to the next idle gif */
        }
    }

    /* Advance the current gif's frames once its delay has elapsed. render_frame()
       loops single-gif states (rewind + replay at end-of-animation). */
    now = now_ms();
    if (now >= g_frameNextAt) {
        g_frameNextAt = now + render_frame();
    }

    draw_hud();
}

/* ── Controls ───────────────────────────────────────────────────── */

/* Apply a preset snapshot + imply a link (mirror the web sim presets). */
static void apply_snapshot(int total, int running, int waiting, const char *msg) {
    g_snap.total   = total;
    g_snap.running = running;
    g_snap.waiting = waiting;
    g_snap.tokens  = 0;
    snprintf(g_snap.msg, sizeof(g_snap.msg), "%s", msg ? msg : "");
    g_linked   = true;
    g_lastSnap = now_ms();
    printf("[pet] heartbeat total=%d run=%d wait=%d msg=\"%s\"\n",
           total, running, waiting, g_snap.msg);
}

enum {
    BTN_IDLE = 1, BTN_BUSY, BTN_ATTENTION, BTN_SLEEP, BTN_TAP, BTN_CONNECT,
};

static lv_obj_t *g_connBtnLabel;   /* toggles Connect/Disconnect text */

static void btn_event_cb(lv_event_t *e) {
    intptr_t which = (intptr_t)lv_event_get_user_data(e);
    switch (which) {
        /* Firmware-preset snapshots (web-sim parity). */
        case BTN_IDLE:      apply_snapshot(1, 0, 0, "idle");                  break;
        case BTN_BUSY:      apply_snapshot(1, 1, 0, "running: Bash");         break;
        case BTN_ATTENTION: apply_snapshot(1, 0, 1, "approve: rm -rf /tmp/x"); break;
        case BTN_SLEEP:     apply_snapshot(0, 0, 0, "all done");             break;
        case BTN_TAP:       tap_heart();                                      break;
        case BTN_CONNECT:
            g_linked = !g_linked;
            if (g_linked) { g_lastSnap = now_ms(); lv_label_set_text(g_connBtnLabel, "disconnect"); }
            else          { lv_label_set_text(g_connBtnLabel, "connect"); printf("[pet] disconnected\n"); }
            break;
    }
}

/* Clicking the pet area also taps -> heart. */
static void panel_click_cb(lv_event_t *e) {
    (void)e;
    tap_heart();
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *txt, intptr_t id,
                             int x, int y, int w, int h) {
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x2B303A), 0);
    lv_obj_add_event_cb(btn, btn_event_cb, LV_EVENT_CLICKED, (void *)id);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, txt);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xDFE4EE), 0);
    lv_obj_center(lbl);
    return btn;
}

static void build_ui(void) {
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x15171C), 0);  /* window chrome */
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_set_style_border_width(screen, 0, 0);

    /* ── The 320x480 device panel (black) ───────────────────────── */
    lv_obj_t *panel = lv_obj_create(screen);
    lv_obj_set_pos(panel, 0, 0);
    lv_obj_set_size(panel, PANEL_W, PANEL_H);
    lv_obj_set_style_bg_color(panel, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_radius(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    /* tap the pet area -> heart */
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(panel, panel_click_cb, LV_EVENT_CLICKED, NULL);

    /* ── The pet (lv_canvas), filling the area above the HUD ─────────
       The AnimatedGIF player decodes + 3x-upscales + centers frames straight
       into PET_CANVAS; we just present that buffer. Sits at (0,0) sized
       320x400, so the 80px HUD below is never overlapped. */
    g_pet = lv_canvas_create(panel);
    lv_canvas_set_buffer(g_pet, PET_CANVAS, CANVAS_W, CANVAS_H, LV_COLOR_FORMAT_RGB565);
    lv_canvas_fill_bg(g_pet, lv_color_hex(COL_BG), LV_OPA_COVER);
    lv_obj_set_pos(g_pet, 0, 0);
    lv_obj_set_size(g_pet, CANVAS_W, CANVAS_H);   /* 320 x 400 */
    lv_obj_set_style_pad_all(g_pet, 0, 0);
    lv_obj_set_style_border_width(g_pet, 0, 0);
    lv_obj_add_flag(g_pet, LV_OBJ_FLAG_EVENT_BUBBLE);        /* clicks bubble to panel -> heart */

    /* ── HUD strip (bottom 80px of the panel) ───────────────────── */
    lv_obj_t *hud = lv_obj_create(panel);
    lv_obj_set_pos(hud, 0, PANEL_H - HUD_H);
    lv_obj_set_size(hud, PANEL_W, HUD_H);
    lv_obj_set_style_bg_color(hud, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(hud, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(hud, 0, 0);
    lv_obj_set_style_pad_all(hud, 0, 0);
    /* top divider line */
    lv_obj_set_style_border_color(hud, lv_color_hex(COL_TEXTDIM), 0);
    lv_obj_set_style_border_width(hud, 1, 0);
    lv_obj_set_style_border_side(hud, LV_BORDER_SIDE_TOP, 0);
    lv_obj_clear_flag(hud, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(hud, LV_OBJ_FLAG_EVENT_BUBBLE);   /* tapping the HUD also -> heart */

    /* pack name (large) */
    g_hudName = lv_label_create(hud);
    lv_label_set_text(g_hudName, PACK_NAME);
    lv_obj_set_style_text_color(g_hudName, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_font(g_hudName, &lv_font_montserrat_28, 0);
    lv_obj_align(g_hudName, LV_ALIGN_TOP_LEFT, 12, 10);

    /* status line */
    g_hudStatus = lv_label_create(hud);
    lv_label_set_text(g_hudStatus, "idle  sim  0:00");
    lv_obj_set_style_text_color(g_hudStatus, lv_color_hex(COL_TEXTDIM), 0);
    lv_obj_set_style_text_font(g_hudStatus, &lv_font_montserrat_14, 0);
    lv_obj_align(g_hudStatus, LV_ALIGN_TOP_LEFT, 12, 48);

    /* connection dot (top-right of the strip): grey/green circle */
    g_dot = lv_obj_create(hud);
    lv_obj_set_size(g_dot, 12, 12);
    lv_obj_align(g_dot, LV_ALIGN_TOP_RIGHT, -12, 14);
    lv_obj_set_style_radius(g_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(g_dot, 0, 0);
    lv_obj_set_style_bg_color(g_dot, lv_color_hex(COL_DOT_OFF), 0);
    lv_obj_set_style_bg_opa(g_dot, LV_OPA_COVER, 0);
    lv_obj_clear_flag(g_dot, LV_OBJ_FLAG_SCROLLABLE);

    /* ── Control strip (bottom 140px of the window) ─────────────── */
    lv_obj_t *ctrl = lv_obj_create(screen);
    lv_obj_set_pos(ctrl, 0, PANEL_H);
    lv_obj_set_size(ctrl, WIN_W, CTRL_H);
    lv_obj_set_style_bg_color(ctrl, lv_color_hex(0x1C1F26), 0);
    lv_obj_set_style_bg_opa(ctrl, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(ctrl, 0, 0);
    lv_obj_set_style_pad_all(ctrl, 0, 0);
    lv_obj_set_style_border_width(ctrl, 0, 0);
    lv_obj_clear_flag(ctrl, LV_OBJ_FLAG_SCROLLABLE);

    /* Row 1: state presets (4 buttons). */
    make_button(ctrl, "idle",      BTN_IDLE,      8,   8, 72, 34);
    make_button(ctrl, "busy",      BTN_BUSY,     84,   8, 72, 34);
    make_button(ctrl, "attention", BTN_ATTENTION, 160, 8, 96, 34);
    make_button(ctrl, "sleep",     BTN_SLEEP,    260,  8, 52, 34);

    /* Row 2: tap + connect toggle. */
    make_button(ctrl, "tap (heart)", BTN_TAP, 8, 50, 140, 34);
    lv_obj_t *conn = make_button(ctrl, "connect", BTN_CONNECT, 156, 50, 156, 34);
    g_connBtnLabel = lv_obj_get_child(conn, 0);

    /* Hint line. */
    lv_obj_t *hint = lv_label_create(ctrl);
    lv_label_set_text(hint,
        "presets imply a link; pet derives state\n"
        "(wait>0 attn, run>0 busy, total=0 sleep)");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x7E8696), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 8, 92);
}

static Uint32 g_last_tick;

static void tick(void *arg) {
    (void)arg;
    Uint32 now = SDL_GetTicks();
    lv_tick_inc(now - g_last_tick);
    g_last_tick = now;
    lv_timer_handler();
}

int main(int argc, char *argv[]) {
    if (argc > 1 && argv[1][0]) {
        snprintf(g_base, sizeof(g_base), "%s", argv[1]);
    }

    setvbuf(stdout, NULL, _IOLBF, 0);
    lv_init();
    lv_display_t *disp = lv_sdl_window_create(WIN_W, WIN_H);
    if (!disp) {
        fprintf(stderr, "lv_sdl_window_create failed\n");
        return 1;
    }
    lv_sdl_window_set_title(disp, "jc3248-pet (sim)");
    lv_sdl_mouse_create();

    g_bootMs = now_ms();
    build_ui();
    enter_state(ST_IDLE);                 /* boot showing idle_0 */
    /* 40ms tick: fast enough to honor per-frame gif delays (~70-120ms) smoothly
       while still cheap. The player itself gates frame advance on the delay. */
    lv_timer_create(pet_timer_cb, 40, NULL);

    printf("pet_sim — %dx%d panel + %dpx controls; gifs from \"%s\"\n",
           PANEL_W, PANEL_H, CTRL_H, g_base);
    printf("pet_sim — AnimatedGIF decoder, 3x nearest-neighbor; click the pet to tap (heart)\n");

    g_last_tick = SDL_GetTicks();

#ifdef __EMSCRIPTEN__
    emscripten_set_main_loop_arg(tick, NULL, 0, 1);
#else
    while (1) {
        tick(NULL);
        SDL_Delay(16);
    }
#endif
    return 0;
}
