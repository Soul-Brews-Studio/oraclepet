/*
 * ble.h — jc3248-pet-idf STEP 9: BLE Nordic UART Service (NUS) bridge.
 *
 * Ported from lab/jc3248-pet/src/ble.{h,cpp} (Arduino BLEDevice) to the
 * ESP-IDF v6 NimBLE host (components/bt/host/nimble). The BleSnapshot struct,
 * the line-assembly ring, blePoll()'s drain/scan/parse, and parseLine() are
 * carried over VERBATIM in behavior; only the transport (Bluedroid->NimBLE)
 * and the JSON extractor (ArduinoJson -> a tiny dependency-free scanner, the
 * same approach gif.cpp's manifest_parse() already uses in this project) changed.
 *
 * Advertises NUS so the Claude desktop app (or nRF Connect) can connect and
 * stream newline-delimited JSON "heartbeat" snapshots. This module ONLY does
 * BLE transport + JSON parsing — no display, no GIF, no HUD. Rendering lives on
 * the main thread, which calls blePoll() each loop.
 *
 * Service UUID  6e400001-b5a3-f393-e0a9-e50e24dcca9e
 * RX char       6e400002-b5a3-f393-e0a9-e50e24dcca9e   (client -> device, WRITE)
 * TX char       6e400003-b5a3-f393-e0a9-e50e24dcca9e   (device -> client, NOTIFY)
 *
 * Open / no-bond: the desktop writes to an open NUS RX char, no pairing.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Latest parsed heartbeat snapshot. Ported VERBATIM from ble.h (C++ struct ->
 * C struct; fields, sizes, and -1/"not provided" semantics identical). */
typedef struct {
    int  total;
    int  running;
    int  waiting;
    long tokens;
    char msg[64];     /* one-line summary ("" if none)                          */
    char prompt[96];  /* "<tool>: <hint>" when a permission prompt is present    */
    long reset5h;     /* seconds until the 5h usage window resets (-1 = absent)  */
    long reset7d;     /* seconds until the 7d (weekly) window resets (-1 = absent)*/
    int  pct5h;       /* 5h window usage percent 0..100 (-1 = absent)            */
    int  pct7d;       /* 7d window usage percent 0..100 (-1 = absent)            */
    int  brightness;  /* backlight 0..255 set from the app (-1 = absent)         */
    long epoch;       /* Unix UTC seconds from the host clock (-1 = absent)      */
    long tzoff;       /* local timezone offset in seconds, e.g. +7h=25200 (0=UTC)*/
} BleSnapshot;

/* Init NimBLE host + controller, register the NUS GATT service, and start
 * advertising as `deviceName` (e.g. "Claude-ECC8"). Spawns the NimBLE host
 * task. Non-fatal on failure: logs and returns; bleConnected() stays false and
 * blePoll() always returns false so the pet runs BLE-less but alive. */
void bleInit(const char *deviceName);

/* True while a central (the desktop) is connected. */
bool bleConnected(void);

/* Non-zero (6-digit) while a pairing passkey should be shown; 0 otherwise.
 * Open chars / no pairing in v1 -> always 0 (kept for HUD-API parity). */
uint32_t blePasskey(void);

/* Call every main-loop iteration. Drains bytes the RX write callback buffered,
 * and when a full '\n'-terminated line has arrived, PARSES it (on THIS calling
 * thread, not the NimBLE host task) into *out and returns true. Returns false if
 * no new complete snapshot this call. If several lines arrived since the last
 * call, *out reflects the newest. */
bool blePoll(BleSnapshot *out);

/* Poll for a connectionless TIME BROADCAST (observer role). When the host's time
 * beacon — BLE local name "clk:<epoch>:<tzoff>" — has been seen since the last
 * call, fills *epoch (UTC seconds) + *tzoff (seconds) and returns true. Works with
 * NO connection (independent of bleConnected()/blePoll()). */
bool blePollBroadcastTime(long *epoch, long *tzoff);

#ifdef __cplusplus
}
#endif
