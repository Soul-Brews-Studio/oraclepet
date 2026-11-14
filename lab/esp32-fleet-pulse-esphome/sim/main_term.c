/*
 * oracle-term — LVGL-WASM replica of the ESPHome terminal viewer (terminal.yaml)
 *
 * Same UI as the JC3248W535: one recolor lv_label (unscii_16, mono), 320x480.
 * The ANSI-SGR -> #RRGGBB recolor markup + UTF-8 -> ASCII mapping is lifted
 * (faithfully) from the device's fetch_pane lambda. Content is pushed from JS
 * via term_set_content() (shell.html fetches maw /api/capture).
 *
 * Native build is SDL2; WASM build is Emscripten+SDL2 (see CMakeLists.txt).
 */
#include "lvgl.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#define SCREEN_W 320
#define SCREEN_H 480
#define LINES_N  22           /* fill ~456px at JetBrains Mono 16 (~21px line) */

/* lv_font_conv'd from the SAME TTFs the device uses (ESPHome gfonts):
 * JetBrains Mono (ASCII 0x20-0x7E) + Sarabun (Thai 0x0E01-0x0E5B), size 16, bpp4. */
extern const lv_font_t term_font_16;

static lv_obj_t *g_term;
static lv_obj_t *g_hdr;
static char g_out[96 * 1024]; /* recolor markup output (content ~16KB, markup ~2-3x) */

/* xterm 16-color basics, matched to the device palette */
static const uint32_t BASIC[8]  = {0x202020,0xCD4848,0x5CB85C,0xD1B357,0x5C94DB,0xB870C7,0x57B8BD,0xC7C7C7};
static const uint32_t BRIGHT[8] = {0x737373,0xF56C66,0x80E080,0xF5DB70,0x80B8F5,0xDB94EB,0x80E6EB,0xF5F5F5};

/* ANSI -> LVGL #RRGGBB recolor markup; span closed per line; '#'->'##'.
 * Direct C port of the fetch_pane lambda (core path; col_off/dash_cap omitted). */
static void build_markup(const char *content)
{
    /* keep only the last LINES_N lines */
    size_t clen = strlen(content);
    size_t start = 0; int nl = 0;
    for (size_t i = clen; i-- > 0; ) {
        if (content[i] == '\n') { if (++nl >= LINES_N) { start = i + 1; break; } }
    }
    const char *p = content + start;

    char *o = g_out; char *oend = g_out + sizeof(g_out) - 16;
    uint32_t cur = 0, span = 0;
    while (*p && o < oend) {
        unsigned char c = (unsigned char)*p;
        if (c == 0x1B) {                                  /* ESC */
            p++;
            if (*p == '[') {
                p++;
                int code[12], n = 0, v = 0; int have = 0;
                while (*p && !(*p >= '@' && *p <= '~')) {
                    if (*p >= '0' && *p <= '9') { v = v*10 + (*p-'0'); have = 1; }
                    else { if (n < 12) code[n++] = have ? v : 0; v = 0; have = 0; }
                    p++;
                }
                char f = *p; if (*p) p++;
                if (f == 'm') {
                    if (have && n < 12) code[n++] = v;
                    if (n == 0) cur = 0;
                    for (int i = 0; i < n; i++) {
                        int cc = code[i];
                        if (cc == 0 || cc == 39) cur = 0;
                        else if (cc >= 30 && cc <= 37) cur = BASIC[cc-30];
                        else if (cc >= 90 && cc <= 97) cur = BRIGHT[cc-90];
                        else if (cc == 38 && i+2 < n && code[i+1] == 5) {
                            int x = code[i+2];
                            if (x < 8) cur = BASIC[x];
                            else if (x < 16) cur = BRIGHT[x-8];
                            else if (x < 232) { int y=x-16; int L[6]={0,95,135,175,215,255}; cur=((uint32_t)L[(y/36)%6]<<16)|((uint32_t)L[(y/6)%6]<<8)|L[y%6]; }
                            else { int g=8+(x-232)*10; cur=((uint32_t)g<<16)|((uint32_t)g<<8)|g; }
                            i += 2;
                        } else if (cc == 38 && i+4 < n && code[i+1] == 2) {
                            cur = ((uint32_t)code[i+2]<<16)|((uint32_t)code[i+3]<<8)|code[i+4]; i += 4;
                        }
                    }
                }
                continue;
            } else if (*p == ']') { while (*p && (unsigned char)*p != 0x07) p++; if (*p) p++; continue; }
            else if (*p) { p++; continue; }
            continue;
        }
        if (c == '\r') { p++; continue; }
        if (c == '\n') { if (span) { *o++='#'; span = 0; } *o++='\n'; p++; continue; }
        if (c < 0x20 && c != '\t') { p++; continue; }
        char ch;
        if (c >= 0x80) {                                  /* UTF-8 -> ASCII lookalike */
            uint32_t cp = c; int len = 1;
            if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
            else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
            else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
            for (int k = 1; k < len && p[k]; k++) cp = (cp << 6) | ((unsigned char)p[k] & 0x3F);
            p += len;
            if (cp==0x2500||cp==0x2501||cp==0x2504||cp==0x2505||cp==0x2508||cp==0x2509||cp==0x254C||cp==0x2014||cp==0x2015) ch='-';
            else if (cp==0x2502||cp==0x2503||cp==0x2506||cp==0x250A||cp==0x2551) ch='|';
            else if (cp>=0x250C && cp<=0x254B) ch='+';
            else if (cp>=0x2580 && cp<=0x259F) ch='#';
            else if (cp==0x25B6||cp==0x25B8||cp==0x25BA||cp==0x23F5||cp==0x2192||cp==0x27A4) ch='>';
            else if (cp==0x25C0||cp==0x25C2||cp==0x25C4||cp==0x2190) ch='<';
            else if (cp==0x2022||cp==0x25CF||cp==0x25CB||cp==0x00B7||cp==0x2219) ch='*';
            else if (cp==0x2713||cp==0x2714||cp==0x2705) ch='v';
            else if (cp==0x2717||cp==0x2718||cp==0x274C) ch='x';
            else if (cp==0x2026) ch='~';
            else if (cp>=0x2800 && cp<=0x28FF) ch=':';
            else ch=' ';
        } else { ch = (char)c; p++; }
        if (span != cur) {
            if (span) *o++='#';
            if (cur) { o += sprintf(o, "#%06X ", (unsigned)cur); }
            span = cur;
        }
        if (ch == '#') { *o++='#'; *o++='#'; } else *o++=ch;
    }
    if (span) *o++='#';
    *o = '\0';
}

static void render(const char *content)
{
    build_markup(content);
    lv_label_set_text(g_term, g_out);
}

/* Exported: JS calls Module.ccall('term_set_content','void',['string'],[txt]) */
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void term_set_content(const char *content) { if (content) render(content); }

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
void term_set_header(const char *txt) { if (txt) lv_label_set_text(g_hdr, txt); }
#endif

static const char *SAMPLE =
  "\x1b[36moracle-term \x1b[39m\x1b[32mLVGL-WASM replica\x1b[39m\n"
  "\x1b[90m  the same recolor pipeline as the device\x1b[39m\n"
  "\n"
  "\x1b[32m+\x1b[39m \x1b[1mpane fix\x1b[22m   maw matches window \x1b[33mname\x1b[39m not index\n"
  "\x1b[32m+\x1b[39m CLIP fix    lv_label.c re-anchors only WRAP\n"
  "\x1b[32m+\x1b[39m perf        \x1b[31m1000ms\x1b[39m -> \x1b[32m455ms\x1b[39m (layout walk)\n"
  "\x1b[34m>\x1b[39m \x1b[35memoji\x1b[39m as colored ascii: \x1b[32m*\x1b[39m \x1b[31mx\x1b[39m \x1b[32mv\x1b[39m\n"
  "\n"
  "\x1b[90m$ \x1b[39mwaiting for maw /api/capture ...\x1b[7m \x1b[27m\n";

static void build_ui(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    g_hdr = lv_label_create(scr);
    lv_obj_set_style_text_font(g_hdr, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(g_hdr, lv_color_hex(0x07E7FF), 0);
    lv_obj_align(g_hdr, LV_ALIGN_TOP_LEFT, 4, 2);
    lv_label_set_text(g_hdr, "oracle-term (sim)");

    g_term = lv_label_create(scr);
    lv_obj_set_pos(g_term, 2, 20);
    lv_obj_set_size(g_term, 316, 456);
    lv_obj_set_style_text_font(g_term, &term_font_16, 0);   /* exact device font */
    lv_obj_set_style_text_color(g_term, lv_color_hex(0xC8C8C8), 0);
    lv_label_set_recolor(g_term, true);
    lv_obj_set_style_text_align(g_term, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_long_mode(g_term, LV_LABEL_LONG_MODE_CLIP);
    render(SAMPLE);
}

static void tick_loop(void)
{
    static Uint32 last = 0;
    Uint32 now = SDL_GetTicks();
    if (last == 0) last = now;
    lv_tick_inc(now - last);
    last = now;
    lv_timer_handler();
}

int main(int argc, char *argv[])
{
    (void)argc; (void)argv;
    setvbuf(stdout, NULL, _IOLBF, 0);
    lv_init();
    lv_display_t *disp = lv_sdl_window_create(SCREEN_W, SCREEN_H);
    if (!disp) { fprintf(stderr, "lv_sdl_window_create failed\n"); return 1; }
    lv_sdl_window_set_title(disp, "oracle-term (sim)");
    build_ui();
    printf("oracle-term sim — %dx%d\n", SCREEN_W, SCREEN_H);
#ifdef __EMSCRIPTEN__
    emscripten_set_main_loop(tick_loop, 0, 1);
#else
    while (1) { tick_loop(); SDL_Delay(16); }
#endif
    return 0;
}
