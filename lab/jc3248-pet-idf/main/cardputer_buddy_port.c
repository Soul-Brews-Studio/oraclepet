/*
 * cardputer_buddy_port.c — native JC port sketch of moremas/build-with-claude.
 *
 * Source inspiration:
 *   /opt/Code/github.com/moremas/build-with-claude/buddy/device/main.py
 *   /opt/Code/github.com/moremas/build-with-claude/buddy/device/buddy_ui_cp.py
 *
 * Why a separate file:
 *   The moremas repo targets M5Stack Cardputer-Adv UIFlow/MicroPython
 *   (240x135, MatrixKeyboard, M5.Lcd, /flash/main.py). Our connected JC board
 *   is ESP32-S3 + AXS15231 QSPI (320x480, touch, native ESP-IDF, LittleFS GIF
 *   pack). Flashing UIFlow/Cardputer firmware onto this panel is the wrong
 *   hardware target, so this module ports the visual/protocol ideas side by side
 *   while keeping the proven frog/HUD firmware alive.
 *
 * Mapping:
 *   Cardputer M5.Lcd.drawString/textWidth  -> hud_gfx 8x8 text primitives
 *   Cardputer 240x135 landscape           -> JC 320x480 portrait
 *   Cardputer MatrixKeyboard Y/N/Q        -> JC touch regions / future BLE cmds
 *   /flash/apps launcher                  -> LittleFS/native app modes
 *   buddy_ble.py NUS JSON                 -> main/ble.c NUS JSON bridge
 *   buddy_ui_cp.py prompt strip           -> this native prompt/status panel
 */
#include "cardputer_buddy_port.h"

#include <stdio.h>
#include <string.h>

#include "display.h"
#include "hud_gfx.h"

#define W DISP_W
#define H DISP_H

/* Anthropic-ish palette translated to RGB565 host order. */
#define C_BLACK   GFX_BLACK
#define C_WHITE   GFX_WHITE
#define C_ORANGE  0xCBAB  /* #CC785C */
#define C_CREAM   0xF79C  /* #F0EEE6 */
#define C_DARK    0x18E3  /* #1F1F1F */
#define C_MID     GFX_GREY
#define C_GREEN   GFX_GREEN
#define C_RED     GFX_RED
#define C_CYAN    GFX_CYAN
#define C_YELLOW  GFX_YELLOW

static uint16_t *fb(void)
{
    return display_framebuffer();
}

static const char *nz(const char *s, const char *fallback)
{
    return (s && s[0]) ? s : fallback;
}

static void text_clipped(uint16_t *p, int x, int y, const char *s, int scale,
                         uint16_t fg, uint32_t bg, int max_px)
{
    if (!s) return;
    char buf[72];
    snprintf(buf, sizeof(buf), "%s", s);
    while (buf[0] && gfx_text_width(buf, scale) > max_px) {
        size_t n = strlen(buf);
        if (n <= 1) break;
        buf[n - 1] = '\0';
    }
    gfx_text(p, W, H, x, y, buf, scale, fg, bg);
}

static void center_text(uint16_t *p, int y, const char *s, int scale,
                        uint16_t fg, uint32_t bg)
{
    int x = (W - gfx_text_width(s, scale)) / 2;
    if (x < 0) x = 0;
    gfx_text(p, W, H, x, y, s, scale, fg, bg);
}

static void draw_header(uint16_t *p, const CardputerBuddyPortState *s)
{
    gfx_fill_rect(p, W, H, 0, 0, W, 34, C_DARK);
    gfx_hline(p, W, H, 0, 34, W, C_ORANGE);
    gfx_text(p, W, H, 10, 9, "Claude Buddy", 2, C_CREAM, GFX_TRANSPARENT);

    const char *conn = nz(s ? s->connection : NULL, "advertising");
    uint16_t dot = (strcmp(conn, "connected") == 0) ? C_GREEN :
                   (strcmp(conn, "disconnected") == 0) ? C_RED : C_ORANGE;
    gfx_fill_circle(p, W, H, W - 22, 17, 7, dot);
    text_clipped(p, W - 126, 12, conn, 1, C_MID, GFX_TRANSPARENT, 90);
}

static void draw_hint_strip(uint16_t *p, int prompt)
{
    gfx_fill_rect(p, W, H, 0, H - 52, W, 52, C_DARK);
    gfx_hline(p, W, H, 0, H - 52, W, C_ORANGE);
    if (prompt) {
        center_text(p, H - 42, "Y once   N deny   Q exit", 1, C_CREAM, GFX_TRANSPARENT);
        center_text(p, H - 24, "touch left / mid / right later", 1, C_MID, GFX_TRANSPARENT);
    } else {
        center_text(p, H - 42, "Waiting for Claude Desktop", 1, C_CREAM, GFX_TRANSPARENT);
        center_text(p, H - 24, "BLE NUS: Claude-XXXX", 1, C_MID, GFX_TRANSPARENT);
    }
}

void cardputer_buddy_port_draw(const CardputerBuddyPortState *state)
{
    uint16_t *p = fb();
    if (!p) return;

    CardputerBuddyPortState empty = {0};
    const CardputerBuddyPortState *s = state ? state : &empty;

    gfx_fill_rect(p, W, H, 0, 0, W, H, C_BLACK);
    draw_header(p, s);

    gfx_rect(p, W, H, 12, 54, W - 24, 86, C_ORANGE);
    text_clipped(p, 24, 68, nz(s->identity, "Buddy"), 2, C_CREAM, GFX_TRANSPARENT, W - 48);
    text_clipped(p, 24, 96, nz(s->owner, "JC native ESP-IDF"), 1, C_MID, GFX_TRANSPARENT, W - 48);
    text_clipped(p, 24, 116, nz(s->status, "idle BLE adv"), 1, C_CYAN, GFX_TRANSPARENT, W - 48);

    char line[72];
    snprintf(line, sizeof(line), "Q:%drun %dwait %dtot", s->running, s->waiting, s->total);
    gfx_text(p, W, H, 24, 170, line, 2, C_CREAM, GFX_TRANSPARENT);
    snprintf(line, sizeof(line), "Today: %d tok", s->tokens_today);
    gfx_text(p, W, H, 24, 204, line, 2, C_CREAM, GFX_TRANSPARENT);

    if (s->has_prompt) {
        gfx_fill_rect(p, W, H, 20, 246, W - 40, 78, C_ORANGE);
        gfx_fill_rect(p, W, H, 24, 250, W - 48, 70, C_BLACK);
        center_text(p, 262, "Permission pending", 1, C_YELLOW, GFX_TRANSPARENT);
        text_clipped(p, 34, 288, nz(s->status, "approve tool?"), 1, C_CREAM, GFX_TRANSPARENT, W - 68);
    } else {
        gfx_rect(p, W, H, 20, 246, W - 40, 78, C_MID);
        center_text(p, 262, "Open Claude", 1, C_CREAM, GFX_TRANSPARENT);
        center_text(p, 288, "Developer > Hardware Buddy", 1, C_MID, GFX_TRANSPARENT);
    }

    draw_hint_strip(p, s->has_prompt);
}

void cardputer_buddy_port_draw_comparison(void)
{
    uint16_t *p = fb();
    if (!p) return;

    gfx_fill_rect(p, W, H, 0, 0, W, H, C_BLACK);
    gfx_fill_rect(p, W, H, 0, 0, W, 34, C_DARK);
    gfx_hline(p, W, H, 0, 34, W, C_ORANGE);
    gfx_text(p, W, H, 8, 9, "Cardputer -> JC port", 2, C_CREAM, GFX_TRANSPARENT);

    const char *left[] = {
        "M5.Lcd 240x135",
        "MatrixKeyboard",
        "/flash/main.py",
        "buddy_ui_cp.py",
        "buddy_ble.py NUS",
        "UIFlow NVS",
    };
    const char *right[] = {
        "AXS15231 320x480",
        "AXS touch regions",
        "ESP-IDF app modes",
        "hud_gfx native panel",
        "main/ble.c NUS",
        "LittleFS + native state",
    };

    int y = 58;
    for (int i = 0; i < 6; i++, y += 54) {
        gfx_fill_rect(p, W, H, 8, y - 8, W - 16, 42, (i & 1) ? 0x0841 : 0x1082);
        text_clipped(p, 16, y, left[i], 1, C_ORANGE, GFX_TRANSPARENT, 128);
        gfx_text(p, W, H, 142, y, "=>", 1, C_MID, GFX_TRANSPARENT);
        text_clipped(p, 166, y, right[i], 1, C_CREAM, GFX_TRANSPARENT, W - 174);
    }

    gfx_fill_rect(p, W, H, 0, H - 58, W, 58, C_DARK);
    gfx_hline(p, W, H, 0, H - 58, W, C_ORANGE);
    center_text(p, H - 46, "Do not flash Cardputer UIFlow", 1, C_YELLOW, GFX_TRANSPARENT);
    center_text(p, H - 26, "Port app ideas into JC native firmware", 1, C_CREAM, GFX_TRANSPARENT);
}
