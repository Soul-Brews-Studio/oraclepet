// JC3248 App Gallery — dispatcher.
// One app per build, selected by -DAPP=N (see Makefile / platformio.ini).
// Common boot: Serial + the PROVEN axs::init() (panel init + non-black warm-up),
// then hand off to the selected app's app_setup()/app_loop().
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <Arduino.h>
#include "axs15231_qspi.h"   // namespace axs (uses lgfx::i2c → include LovyanGFX first)
#include "app_api.h"

#ifndef APP
#define APP 0
#endif

#include "apps/index.h"      // pulls in every app header; only APP==N compiles

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.printf("\n=== JC3248 App Gallery — APP %03d ===\n", APP);
  // Full ~10s colour warm-up only for app 000 (the baseline / cold-boot proof).
  // Apps 001+ get the fast ~1.5s settle so each bisection flash isn't 10s of
  // colour cycle — we've proven the panel holds, so the loop is just wasted time.
  axs::init(/*warmup=*/ APP == 0);
  // Clean BLACK slate so the settle/warm-up is visually distinct from the app's
  // content (and a white screen = broken layer is unambiguous).
  axs::fill_solid(0x0000);
  delay(400);
  Serial.printf(">>> warm-up done — APP %03d rendering now\n", APP);
  app_setup();
}

void loop() {
  app_loop();
}
