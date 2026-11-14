/*
 * cardputer_buddy_port.h — JC3248/JC4832 side-by-side port sketch for
 * moremas/build-with-claude's Cardputer-Adv MicroPython Buddy UI.
 *
 * This intentionally lives in a NEW file so the proven jc3248-pet GIF/HUD path
 * stays intact. It gives us a native ESP-IDF rendering seam for comparing the
 * Cardputer MicroPython layout against our AXS15231 board without flashing the
 * wrong UIFlow firmware onto the JC display.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *connection;  /* advertising, connected, disconnected */
    const char *identity;    /* device/persona name */
    const char *owner;       /* optional owner line */
    const char *status;      /* live status/prompt text */
    int running;
    int waiting;
    int total;
    int tokens_today;
    int has_prompt;
} CardputerBuddyPortState;

/*
 * Render a Cardputer-inspired Buddy screen into display.c's framebuffer.
 * This does not flush; caller may display_flush() when ready.
 */
void cardputer_buddy_port_draw(const CardputerBuddyPortState *state);

/*
 * Render a compact side-by-side comparison page explaining how the MicroPython
 * Cardputer bundle maps to this board's native ESP-IDF pieces.
 */
void cardputer_buddy_port_draw_comparison(void);

#ifdef __cplusplus
}
#endif
