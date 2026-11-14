/*
 * ota.h — jc3248-pet-idf STEP 11: firmware update over BLE (nRF52-DFU style).
 *
 * A second GATT service next to NUS. The host (Desk Pet app) streams the new
 * app .bin; the pet writes it into the idle OTA slot, checks it, and reboots
 * into it. Rollback is on: the new image must call ota_mark_valid() (main.c does,
 * once the display and BLE are up) or the bootloader returns to the old slot.
 *
 * Service  6e400010-b5a3-f393-e0a9-e50e24dcca9e  "Pet OTA"
 *   control 6e400011-…  READ   -> {"ver":"…","slot":"ota_0","project":"…","sha":"<16 hex of the ELF SHA-256>"}
 *                       WRITE  <- commands (below), with response
 *                       NOTIFY -> JSON status lines
 *   data    6e400012-…  WRITE WITHOUT RESPONSE <- raw image bytes, in order
 *
 * Commands on control (little-endian):
 *   0x01 BEGIN  u32 size, u32 crc32 (zlib CRC-32 of the whole image)
 *   0x02 END    the pet finishes writing, verifies, switches slot, reboots
 *   0x03 ABORT
 *   0x04 BEGIN_FILE  u32 size, u32 crc32, u8 len, path   partial OTA: one file into
 *                    /littlefs ("characters/<pack>/<file>"), no reboot; ends {"ota":"saved"}
 *   0x05 SELECT_PACK u8 len, name   show that pack now and after reboots; {"ota":"pack"}
 *   0x08 PLAY        u8 len, name   play /littlefs/sounds/<name>.ima (sent as a file,
 *                    "sounds/<name>.ima": PSND IMA ADPCM 16 kHz); {"ota":"play","name":…}
 *   0x0B BEGIN_STREAM u32 size, u32 crc32   a PSND clip played WHILE it arrives (data char,
 *                    same window/acks); nothing is stored; ends {"ota":"streamed","crc":…}
 *   0x0C LISTEN      u16 ms, u8 flags   record the mic; {"ota":"heard","rms":…,"freq":…,"db":…};
 *                    flags bit 0: the audio follows as a PSND stream on the mic char 6e400013 (notify)
 *   0x0A SELFTEST    beep 1 kHz; boards with a mic record it: {"ota":"selftest","mic":…,"heard":…,"db":…}
 *   0x07 PROGRESS    u8 pct   the host's overall % of a batch (packs, apps), drawn on the pet
 *   0x06 RUN_APP     u8 len, name   run /littlefs/apps/<name>.wasm (sent as a file,
 *                    "apps/<name>.wasm"); len 0 stops the app; {"ota":"app"}
 *
 * Notifications on control (one JSON object each):
 *   {"ota":"ready","size":N,"window":W}   send at most W bytes beyond the last ack
 *   {"ota":"ack","off":N}                 N bytes taken off the link so far
 *   {"ota":"done"}                        image accepted; rebooting in ~1 s
 *   {"ota":"saved","path":"…"}            a file transfer landed (partial OTA)
 *   {"ota":"pack","name":"…"}             the pet now shows that pack
 *   {"ota":"error","why":"…"}             transfer stopped; the old app keeps running
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* GATT service definition for ble.c to register alongside NUS. */
const void *ota_gatt_svcs(void);   /* const struct ble_gatt_svc_def[] */

/* Hooks from ble.c's GAP handler. */
void ota_on_connect(uint16_t conn_handle);
void ota_on_disconnect(void);
void ota_on_subscribe(uint16_t attr_handle, bool notify);

/* Main thread: a SELECT_PACK arrived (copies the name; true once per request),
 * and its outcome, which is notified to the host. */
bool ota_take_pack_request(char *out, size_t n);
bool ota_take_app_request(char *out, size_t n);
void ota_app_result(const char *name, bool ok, const char *why);
void ota_pack_result(const char *name, bool ok);

/* Main-thread view: a pack/app transfer is running; the % the host shows (or this file's). */
bool ota_file_progress(int *percent);

/* Main-thread view: an app update is in progress and how far it got (0..100). */
bool ota_active(int *percent);

/* Mark the running image good (cancels rollback). Safe to call every boot. */
void ota_mark_valid(void);
void ota_wifi_note(const char *state);   /* wifi.c: tell the host the WiFi state */

#ifdef __cplusplus
}
#endif
