/*
 * pet.h — the pet SDK: write a desk-pet app in C, compile it to WebAssembly, and
 * the jc3248 pet runs it on top of its firmware (the "pet OS", main/petapp.c).
 *
 *   ./build.sh examples/hello.c          # -> build/hello.wasm (a few hundred bytes)
 *   cd ../desk-pet-mac && just app ../pet-sdk/build/hello.wasm   # partial OTA + run
 *
 * The firmware keeps the hardware: display, GIF engine, BLE, clock, OTA. Your app
 * only says what the pet should do. No libc: freestanding wasm32, no malloc needed.
 * Every callback must return within 2 s or the pet restarts without your app.
 */
#pragma once

#define PET_IMPORT(n) __attribute__((import_module("pet"), import_name(n)))
#define PET_EXPORT(n) __attribute__((export_name(n)))

/* What the pet shows (the current pack's state GIFs). */
enum { PET_IDLE = 0, PET_BUSY = 1, PET_ATTENTION = 2, PET_SLEEP = 3 };
/* Sounds. */
enum { PET_RIBBIT = 0, PET_BLIP_UP = 1, PET_BLIP_DOWN = 2 };

PET_IMPORT("show")    void pet_show(int state);
PET_IMPORT("react")   void pet_react(void);                       /* the heart */
PET_IMPORT("caption") void pet_caption_n(const char *s, int len); /* line under the pet, 20 chars */
PET_IMPORT("status")  void pet_status_n(const char *s, int len);  /* HUD status line */
PET_IMPORT("pack")    int  pet_pack_n(const char *s, int len);    /* switch animation pack; 1 = ok */
PET_IMPORT("sound")   void pet_sound(int kind);
PET_IMPORT("millis")  int  pet_millis(void);
PET_IMPORT("log")     void pet_log_n(const char *s, int len);     /* serial console */
PET_IMPORT("tone")    void pet_tone(int hz, int ms);              /* 50..8000 Hz, 10..2000 ms */
PET_IMPORT("play")    int  pet_play_n(const char *s, int len);    /* a clip on the pet (sounds/<name>.ima); 1 = playing */
PET_IMPORT("say")     int  pet_say_n(const char *s, int len);     /* speak text (OraclePet makes the voice); 1 = asked */

/* Canvas mode: the app draws its own screen (the area above the HUD) instead of the cat.
 * Colours are RGB565 (pet_rgb). The pet shows each finished callback's drawing. Needs pet
 * firmware with canvas mode (2026-10-21 on); older firmware stops the app at the first call. */
PET_IMPORT("canvas")  void pet_canvas(int on);                    /* 1: canvas mode, 0: back to the cat */
PET_IMPORT("clear")   void pet_clear(int color);
PET_IMPORT("rect")    void pet_rect(int x, int y, int w, int h, int color);    /* filled */
PET_IMPORT("circle")  void pet_circle(int x, int y, int r, int color);         /* filled */
PET_IMPORT("line")    void pet_line(int x0, int y0, int x1, int y1, int color);
PET_IMPORT("text")    void pet_text_n(int x, int y, int scale, int color, const char *s, int len);  /* 8x8 font x scale, ASCII */
PET_IMPORT("width")   int  pet_width(void);                       /* canvas size: 320 x 400 (3.5in), 240 x 244 (2.8in) */
PET_IMPORT("height")  int  pet_height(void);
static inline int pet_rgb(int r, int g, int b) { return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b & 0xFF) >> 3); }

static inline int pet_strlen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static inline void pet_caption(const char *s) { pet_caption_n(s, pet_strlen(s)); }
static inline void pet_status(const char *s)  { pet_status_n(s, pet_strlen(s)); }
static inline int  pet_pack(const char *s)    { return pet_pack_n(s, pet_strlen(s)); }
static inline void pet_log(const char *s)     { pet_log_n(s, pet_strlen(s)); }
static inline int  pet_play(const char *s)    { return pet_play_n(s, pet_strlen(s)); }
static inline int  pet_say(const char *s)     { return pet_say_n(s, pet_strlen(s)); }
static inline void pet_text(int x, int y, int scale, int color, const char *s) { pet_text_n(x, y, scale, color, s, pet_strlen(s)); }

/* Tiny text helper: append an int to buf at *at (no libc). */
static inline void pet_put_int(char *buf, int *at, int v)
{
    char t[12]; int n = 0;
    if (v < 0) { buf[(*at)++] = '-'; v = -v; }
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) buf[(*at)++] = t[--n];
    buf[*at] = 0;
}
static inline void pet_put_str(char *buf, int *at, const char *s) { while (*s) buf[(*at)++] = *s++; buf[*at] = 0; }

/*
 * Callbacks you may export (all optional):
 *
 *   PET_EXPORT("on_start") void on_start(void);
 *   PET_EXPORT("on_tick")  void on_tick(int ms);                 about every 100 ms
 *   PET_EXPORT("on_tap")   void on_tap(int x, int y);            replaces the default heart
 *   PET_EXPORT("on_fleet") void on_fleet(int total, int running, int waiting);
 *                          each heartbeat; exporting it means YOUR app decides the state
 */
