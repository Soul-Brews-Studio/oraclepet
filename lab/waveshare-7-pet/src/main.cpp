// waveshare-7-pet — the jc3248-pet desk-pet, ported to the Waveshare
// ESP32-S3-Touch-LCD-7 (800×480 RGB). SAME ESP32-S3 chip → the pet's brain
// (pet.cpp state machine + AnimatedGIF + ble.cpp) is reused VERBATIM; only the
// display HAL changed (axs15231_qspi.h → ws_7inch_rgb.h). "Same soul, new body."
//
// TU rule (unchanged from jc3248-pet): the axs:: driver keeps STATIC state, so
// ONLY this file calls axs::init/push/fill_solid/read_touch. pet.cpp draws into
// the shared `spr` (320×480); ws_7inch_rgb.h centers it on the 800×480 canvas.
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <Arduino.h>
#include "ws_7inch_rgb.h"    // namespace axs (RGB HAL) — was axs15231_qspi.h
#include "pet.h"             // extern spr; pet_setup/pet_tick/pet_on_touch/pet_set_*
#include "ble.h"             // bleInit/bleConnected/blePasskey/blePoll + BleSnapshot

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== waveshare-7-pet === boot (GIF pet + BLE on 800x480 RGB)");
  if (!axs::init(/*warmup=*/false)) // CH422G + RGB panel + GT911 reset/probe
    Serial.println("[main] FATAL: axs::init() failed — display will be dead (see [ws7] log above)");
  axs::set_backlight(1);         // ensure backlight on (CH422G 0x1E) — workflow flagged this
  axs::fill_solid(0x0000);       // black slate
  pet_setup();                   // creates spr (320x480) FIRST, mounts FS, loads bufo, frame 0
  axs::push((const uint16_t*)spr.getBuffer());   // centered on the 800x480 canvas
  Serial.println("[pet] first frame pushed");

  // BLE bridge AFTER the sprite is allocated. Advertise NUS as "Claude-XXXX".
  char name[16];
  uint64_t mac = ESP.getEfuseMac();
  snprintf(name, sizeof(name), "Claude-%04X", (unsigned)(mac & 0xFFFF));
  bleInit(name);
}

void loop() {
  // BLE: pull any new heartbeat + link/passkey into the pet's HUD.
  BleSnapshot snap;
  if (blePoll(&snap)) pet_set_activity(snap.total, snap.running, snap.waiting, snap.msg, snap.tokens);
  pet_set_link(bleConnected(), blePasskey());

  // Touch (only this TU may call the axs:: driver).
  int16_t tx, ty;
  if (axs::read_touch(&tx, &ty)) pet_on_touch();

  if (pet_tick()) {              // decoded a new frame into spr?
    axs::push((const uint16_t*)spr.getBuffer());
  }
}
