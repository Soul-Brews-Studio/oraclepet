#pragma once
#include <stdint.h>

// ---------------------------------------------------------------------------
// BLE Nordic UART Service bridge for the JC3248 desk-pet.
//
// Advertises NUS so a Claude desktop app (or nRF Connect) can connect and
// stream newline-delimited JSON "heartbeat" snapshots (see REFERENCE.md). This
// module ONLY does BLE transport + JSON parsing — no display, no sprite, no
// LovyanGFX. Rendering lives elsewhere on the main thread.
//
// Service UUID  6e400001-b5a3-f393-e0a9-e50e24dcca9e
// RX char       6e400002-b5a3-f393-e0a9-e50e24dcca9e   (client → device, WRITE)
// TX char       6e400003-b5a3-f393-e0a9-e50e24dcca9e   (device → client, NOTIFY)
// ---------------------------------------------------------------------------

// Latest parsed heartbeat snapshot (see REFERENCE.md).
struct BleSnapshot {
  int  total;
  int  running;
  int  waiting;
  long tokens;
  char msg[64];     // one-line summary ("" if none)
  char prompt[96];  // "<tool>: <hint>" when a permission prompt is present, else ""
};

void     bleInit(const char* deviceName);   // init Bluedroid, NUS service, start advertising
bool     bleConnected();                     // a central is connected
uint32_t blePasskey();                       // non-zero (6-digit) while a pairing passkey should be shown; 0 otherwise
// Call every main-loop iteration. Drains bytes received by the RX callback,
// and when a full '\n'-terminated line has arrived, PARSES it (on THIS calling
// thread, not the BLE task) into *out and returns true. Returns false if no new
// complete snapshot this call. (Keeps ArduinoJson off the BLE callback thread.)
bool     blePoll(BleSnapshot* out);
