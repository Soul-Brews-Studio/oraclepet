/*
 * petapp.h — jc3248-pet-idf STEP 12: pet apps on WebAssembly (the "pet OS").
 *
 * The firmware owns the hardware (display, GIF engine, BLE, clock, OTA). A pet
 * app is a small .wasm module (built with lab/pet-sdk) that lives in
 * /littlefs/apps/<name>.wasm, arrives by partial OTA, and drives the pet only
 * through the host API below. Swapping an app never touches the firmware.
 *
 * Host API (import module "pet"; see lab/pet-sdk/pet.h):
 *   show(state)            0 idle, 1 busy, 2 attention, 3 sleep
 *   react()                the tap reaction (heart)
 *   caption(ptr, len)      the line under the pet ("" clears it)
 *   status(ptr, len)       the HUD status line
 *   pack(ptr, len) -> ok   switch animation pack
 *   sound(kind)            0 ribbit, 1 blip up, 2 blip down
 *   millis() -> ms         uptime
 *   log(ptr, len)          serial console
 *   canvas(on)             canvas mode: the app draws the screen above the HUD (the cat pauses)
 *   clear(c), rect(x, y, w, h, c), circle(x, y, r, c), line(x0, y0, x1, y1, c),
 *   text(x, y, scale, c, ptr, len)   drawing, RGB565 colours; width() / height() = the canvas
 *
 * App exports (all optional):
 *   on_start()                         after load
 *   on_tick(ms)                        about every 100 ms
 *   on_tap(x, y)                       the screen was touched (replaces the default heart)
 *   on_fleet(total, running, waiting)  each heartbeat (the app then owns the pet's state)
 *
 * Safety: every call runs under a 2 s watchdog. An app that overruns restarts
 * the pet with the app marked bad, and the next boot runs without it. After a
 * panic or watchdog reset the saved app is not started either.
 * WAMR runs on its own pthread (see petapp.c for why).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Start the runtime and the app saved in NVS (unless it crashed last time). */
void petapp_boot(void);

/* Ask the app thread to run /littlefs/apps/<name>.wasm ("" stops the app). The
 * outcome is notified to the BLE host (ota_app_result) from the app thread. */
bool petapp_run(const char *name);

/* Apply what the app asked for (show, caption, pack, ...). Main loop, every pass. */
void petapp_poll(void);

/* Main-loop hooks. Each returns true when the app handled the event. */
void petapp_tick(void);
bool petapp_tap(int x, int y);
bool petapp_fleet(int total, int running, int waiting);

/* The running app takes over the pet's state / caption (main.c skips its own). */
bool petapp_owns_state(void);
/* Canvas mode (pet_canvas): the main loop shows the app's drawing instead of the cat.
 * present() copies a finished frame into fb (true = it changed); left() is true once after
 * canvas mode ends, so the cat's area can be repainted. */
#include <stdint.h>
bool petapp_canvas_active(void);
bool petapp_canvas_present(uint16_t *fb);
bool petapp_canvas_left(void);
bool petapp_owns_caption(void);

/* Name of the running app ("" when none), and the last error for the host. */
const char *petapp_name(void);
const char *petapp_error(void);

#ifdef __cplusplus
}
#endif
