#pragma once
// jc3248-pet — audio TU (audio.h)
//
// Self-contained I2S TX cue player. Mirrors ble.cpp: owns its peripheral
// statically and NEVER touches the axs:: display driver or the shared sprite.
// main.cpp is the only caller. Generates short tones (no asset, no decoder,
// no SD card) on the board's always-on NS4168/MAX98357A-class amp.
//
// Hardware (verified, NorthernMan54/JC3248W535EN vendor demo + schematic):
//   I2S TX  BCK=GPIO42  LRCK/WS=GPIO2  DIN/DOUT=GPIO41  MCK=none(-1)
//   Amp enable: NONE — NS4168 CTRL/SD (net SD_MD) tied HIGH via 1M (R23),
//   not on any GPIO. Amp is always-on, gated only by I2S data. No pin to drive.
//
// LOUDNESS: AUDIO_VOL (in audio.cpp) is the master volume, 0.0..1.0. It is set
// LOUD by default. The NS4168 can push ~3W — loud playback draws a current
// spike that can brown-out-reset the board on a weak USB port (the exact "plug
// speaker -> not boot" failure). If it resets on a tap: power from a LiPo / a
// strong >=1A 5V supply, OR lower AUDIO_VOL.

#include <stdint.h>

// Configure I2S TX once. Call LAST in setup() (after bleInit) so DMA buffers
// (internal RAM) allocate after the 307KB framebuffer + Bluedroid. Idempotent.
void audioInit();

// Brighter/dimmer cue: a single blip. up=true -> higher pitch. ~150 ms, blocking.
void audioBlip(bool up);

// Heart-tap cue: a loud, longer 3-croak "ribbit". ~700 ms, blocking. The GIF
// pauses for the burst (I2S write is blocking) — fine for a deliberate tap.
void audioRibbit();
