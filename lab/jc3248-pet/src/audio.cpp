#include "audio.h"
#include <Arduino.h>
#include <ESP_I2S.h>     // built-in: arduino-esp32 3.x (framework-arduinoespressif32/libraries/ESP_I2S)
#include <math.h>

// ---- I2S pin map (this board) — kept private to this TU ----
// IMPORTANT: GPIO41 is the ESP32 DATA-OUT to the amp, so it goes in the DOUT
// slot of setPins(bclk, ws, dout, din, mclk). Passing it as din = total silence.
static constexpr int8_t  PIN_BCK  = 42;   // BCLK
static constexpr int8_t  PIN_WS   = 2;    // LRCK / word select
static constexpr int8_t  PIN_DOUT = 41;   // DIN on the amp = ESP32 data-out
static constexpr int8_t  PIN_DIN  = -1;   // TX only
static constexpr int8_t  PIN_MCK  = -1;   // NS4168 needs no MCLK

static constexpr uint32_t SR  = 16000;    // 16 kHz mono
static constexpr float    PI2 = 6.28318531f;

// MASTER VOLUME + HARD CEILING. Output power scales as VOL^2; the NS4168 browns
// out a 500mA USB-2 budget above ~0.35 (0.50 ~576mA, 1.0 ~1.5A = "amp -> no boot").
// 0.25 is the max-safe-on-USB with wide margin. For ear-splitting loud, run from
// the onboard LiPo / a >=1A supply and raise BOTH constants.
static constexpr float VOL_MAX   = 0.25f;
static constexpr float AUDIO_VOL = 0.25f;
static const     float VOL  = (AUDIO_VOL < VOL_MAX) ? AUDIO_VOL : VOL_MAX;
static const     int16_t PEAK = (int16_t)(0x7FFF * VOL);

static I2SClass s_i2s;
static bool     s_ready = false;

static inline void emit(const int16_t* b, uint32_t n) { s_i2s.write((uint8_t*)b, n * 2); }

// Soft clip adds grit (odd harmonics) and reads louder on a small speaker.
static inline float softclip(float x) {
  if (x >  1.0f) x =  1.0f;
  if (x < -1.0f) x = -1.0f;
  return 1.5f * x - 0.5f * x * x * x;
}

// In-band single tone (the blips). ~5 ms attack/decay kills the click-pop.
static void tone(float f, uint32_t ms) {
  if (!s_ready) return;
  const uint32_t n = SR * ms / 1000, fade = SR / 200;
  const float w = PI2 * f / (float)SR;
  int16_t buf[256]; uint32_t i = 0, left = n;
  while (left) {
    uint32_t c = left < 256 ? left : 256;
    for (uint32_t k = 0; k < c; k++, i++) {
      float env = 1.0f;
      if (i < fade)          env = (float)i / fade;
      else if (i > n - fade) env = (float)(n - i) / fade;
      buf[k] = (int16_t)(PEAK * env * sinf(w * (float)i));
    }
    emit(buf, c); left -= c;
  }
}

// One frog syllable: carrier glides f0->f1 (kept in the speaker's passband),
// amplitude-modulated at modHz, with 2nd/3rd harmonics + soft clip — a buzzy
// croak the tiny BTL driver can actually radiate. Phase-integrated for the glide.
static void syllable(float f0, float f1, float modHz, uint32_t ms) {
  if (!s_ready) return;
  const uint32_t n = SR * ms / 1000, fade = SR / 150;
  const float wm = PI2 * modHz / (float)SR;
  float ph = 0.0f;
  int16_t buf[256]; uint32_t i = 0, left = n;
  while (left) {
    uint32_t c = left < 256 ? left : 256;
    for (uint32_t k = 0; k < c; k++, i++) {
      float t  = (float)i / (float)n;
      float fc = f0 + (f1 - f0) * t;          // carrier glide
      ph += PI2 * fc / (float)SR;
      float harm = sinf(ph) + 0.4f * sinf(2.0f * ph) + 0.2f * sinf(3.0f * ph);
      float mod  = 0.5f + 0.5f * sinf(wm * (float)i);   // AM rattle
      float env  = 1.0f;
      if (i < fade)          env = (float)i / fade;
      else if (i > n - fade) env = (float)(n - i) / fade;
      buf[k] = (int16_t)(PEAK * softclip(harm * 0.7f) * mod * env);
    }
    emit(buf, c); left -= c;
  }
}

static void gap(uint32_t ms) {
  if (!s_ready) return;
  const uint32_t n = SR * ms / 1000;
  int16_t z[256] = {0}; uint32_t left = n;
  while (left) { uint32_t c = left < 256 ? left : 256; emit(z, c); left -= c; }
}

void audioInit() {
  if (s_ready) return;
  s_i2s.setPins(PIN_BCK, PIN_WS, PIN_DOUT, PIN_DIN, PIN_MCK);  // GPIO41 in DOUT slot
  if (!s_i2s.begin(I2S_MODE_STD, SR, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO)) {
    Serial.println("[snd] I2S begin FAILED");
    return;
  }
  s_ready = true;
  Serial.printf("[snd] I2S TX ready (16k/16/mono BCK42/WS2/DOUT41, vol=%d%% max=%d%%, amp always-on)\n",
                (int)(VOL * 100), (int)(VOL_MAX * 100));
}

void audioBlip(bool up) {
  tone(up ? 1000.0f : 750.0f, 80);   // in-band, distinct higher/lower
}

void audioRibbit() {
  // "rib-bit" = high syllable + lower syllable; repeated 3x for a longer call
  // (~740 ms total). All carriers stay in the speaker's passband so it's audible.
  for (int r = 0; r < 3; r++) {
    syllable(520, 360, 55, 70);   // "rib"
    gap(22);
    syllable(430, 300, 52, 95);   // "bit"
    if (r < 2) gap(90);           // pause between calls
  }
}
