/*
 * gif.h — jc3248-pet-idf STEP 6: animated-GIF renderer (the headline).
 *
 * Ported from the proven Arduino renderer lab/jc3248-pet/src/pet.cpp:
 *   - open a bufo character pack from LittleFS (/littlefs/characters/<name>/)
 *   - decode frames with Larry Bank's AnimatedGIF (vendored as a local component)
 *   - 3x nearest-neighbor upscale + center each frame into the EXISTING display
 *     PSRAM framebuffer (display.c's s_fb, via display_framebuffer()) — NO second
 *     framebuffer is allocated here
 *   - pace frames by the per-frame GIF delay (esp_timer), rotate idle_0..N forever
 *   - push to the panel via display_flush() (esp_lcd_panel_draw_bitmap)
 *
 * Implementation is C++ (gif.cpp) because AnimatedGIF is a C++ class; this header
 * is a plain C API so it can be called from jc3248_pet_idf_main.c.
 *
 * API shape (mirrors pet_setup()/pet_tick()):
 *   gif_setup() once after display_init() (display fb must already exist), then
 *   gif_tick() every loop iteration — it self-paces (returns fast when the frame
 *   isn't due yet) and only pushes the panel when it actually composited a frame.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Mount point that step 5 (LittleFS) registers the storage partition under.
 * The renderer looks for packs at GIF_BASE_DIR "/<name>" — first dir found wins,
 * matching pet.cpp's "scan /characters for the first directory" behavior.
 */
#define GIF_MOUNT     "/littlefs"
#define GIF_BASE_DIR  GIF_MOUNT "/characters"

/*
 * Initialize the GIF renderer:
 *   - requires display_init() to have already run (uses display_framebuffer()).
 *   - scans GIF_BASE_DIR for the first character pack directory, reads its
 *     manifest.json (idle list + reaction/state GIFs + bg color), gif.begin(),
 *     opens the first idle GIF and decodes frame 0.
 *
 * Returns:
 *   ESP_OK            — pack loaded, frame 0 on screen.
 *   ESP_ERR_INVALID_STATE — display framebuffer not allocated (call display_init first).
 *   ESP_ERR_NOT_FOUND — no pack / manifest / idle state under GIF_BASE_DIR.
 *   ESP_FAIL          — manifest parse or first-GIF open failed.
 * Non-fatal to the app: on failure the caller can keep the test pattern on screen.
 */
esp_err_t gif_setup(void);

/*
 * Advance the animation by at most one frame and push if something changed.
 * Self-pacing: cheap (no decode, no push) when the current frame's delay hasn't
 * elapsed. Call it every main-loop iteration. Returns true iff a frame was
 * composited + flushed this call (useful if a HUD wants to compose afterward).
 * No-op returning false if gif_setup() did not succeed.
 */
bool gif_tick(void);

/*
 * Tap reaction: switch to the pack's "heart" (else "celebrate") GIF, looped, for
 * a few seconds, then fall back to idle rotation. Safe to call from the touch
 * handler. No-op if no reaction GIF in the manifest. Mirrors pet_on_touch().
 */
void gif_on_touch(void);

/* Activity from the BLE heartbeat, same rule as pet.cpp computeDesired and the web sim:
 * waiting>0 -> ATTENTION, running>0 -> BUSY, total==0 -> SLEEP, else IDLE; no or stale
 * link -> IDLE. Plays the pack's busy/attention/sleep GIF (idle if the pack lacks it). */
#define PET_ACT_IDLE      0
#define PET_ACT_BUSY      1
#define PET_ACT_ATTENTION 2
#define PET_ACT_SLEEP     3
void gif_set_activity(int act);

/*
 * The pack's background color (RGB565, host order) from manifest colors.bg, or
 * 0x0000 until/if gif_setup() ran. Step 7 uses this so the HUD clears its bottom
 * strip to the SAME background as the frog's canvas (one seamless backdrop).
 */
uint16_t gif_bg(void);

/*
 * The loaded character-pack name (e.g. "bufo"), or "" if no pack loaded. Step 7
 * shows this on HUD line 1 (mirrors pet.cpp's petName, derived from the pack dir).
 */
const char *gif_pack_name(void);

/* The line under the pet: the oracle that is working or waiting (from the
 * heartbeat's "who"). "" draws nothing. Replaces the old fixed signature. */
void gif_set_who(const char *who);

/* STEP 11 (partial OTA): show another pack from /littlefs/characters now and
 * keep it across reboots (NVS "pet"/"pack"). False if it does not load; the
 * previous pack stays. Main thread only. */
bool gif_select_pack(const char *name);

#ifdef __cplusplus
}
#endif
