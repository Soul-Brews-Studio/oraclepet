#pragma once
/*
 * jc3248-pet-idf — STEP 8: audio (the ribbit) — audio.h
 *
 * NS4168 I2S class-D amp, always-on (no enable GPIO). 16 kHz / 16-bit / mono.
 * Pins: BCK=GPIO42, WS/LRCK=GPIO2, DOUT=GPIO41 (ESP32 data-out → amp DIN), no MCLK.
 *
 * Ported VERBATIM (tone/syllable/croak/gap/softclip DSP + VOL/PEAK ceiling) from
 * the Arduino body lab/jc3248-pet/src/audio.cpp — only the I2S transport changed
 * from arduino-esp32's ESP_I2S I2SClass to native ESP-IDF i2s_std (driver/i2s_std.h,
 * component esp_driver_i2s).
 *
 * POWER: NS4168 browns out a 500mA USB-2 budget above ~VOL 0.35; the master VOL
 * is hard-capped at 0.25 (VOL_MAX). The DSP keeps an attack/decay envelope so the
 * blips don't click-pop. For ear-splitting loud, run off LiPo / >=1A and raise both.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up the i2s_std TX channel (16k/16/mono, BCK42/WS2/DOUT41). Idempotent. */
void audio_init(void);

/* The frog call: high+low syllable "rib-bit", repeated 3x (~740 ms). */
void audio_ribbit(void);

/* UI blip — in-band tone, higher (up) or lower (down). */
void audio_blip(bool up);

/* A plain tone: hz 50..8000, ms 10..2000 (queued, returns at once). */
void audio_tone(int hz, int ms);

/* Queue /littlefs/sounds/<name>.ima (PSND IMA ADPCM, 16 kHz). False if the queue is full.
 * Any task may call it; playback runs on the audio task. */
bool audio_play(const char *name);

/* Speaker self-test: plays a 1 kHz tone (1 s). Boards with a mic (lcd28) record it and
 * report whether it was heard: done(has_mic, heard, db_1k, rms_quiet, rms_tone), called
 * on the audio task. */
typedef void (*audio_selftest_cb_t)(bool has_mic, bool heard, float db, int rms_quiet, int rms_tone);
bool audio_selftest(audio_selftest_cb_t done);

/* Live stream: a PSND clip played while it arrives. begin() queues the player; write()
 * feeds bytes and blocks up to wait_ms when the ~1 s buffer is full (that backpressure
 * is what slows the BLE sender); end() marks the last byte. Returns bytes accepted. */
typedef struct {
    bool     has_mic, sent;
    int      ms, rms, peak, freq, floor;
    uint32_t samples;
    float    db;
} audio_listen_result_t;
typedef void (*audio_listen_cb_t)(const audio_listen_result_t *r);
typedef bool (*audio_chunk_cb_t)(const uint8_t *d, size_t n);

/* LISTEN: record the mic for ms (16 kHz), measure it (RMS, peak, floor, strongest frequency
 * 150-4000 Hz and how far it stands out), and with send set also hand the recording to
 * chunk() as a PSND IMA stream (16-byte header, then 256-byte blocks). Runs on the audio task;
 * done() gets the result. Boards without a mic report has_mic = false. */
bool audio_listen(int ms, bool send, audio_listen_cb_t done, audio_chunk_cb_t chunk);

/* Voice mode (lab/oraclepet-lib/VOICE.md): when nothing plays, the audio task keeps the mic
 * open and an energy VAD cuts it into utterances: start(), then block() for every 256-byte
 * PSND block (self-contained IMA ADPCM, 504 samples), then end() with the exact sample count.
 * The callbacks run on the audio task. NULL turns voice mode off. Only boards with a mic. */
typedef struct {
    void (*start)(int id, int floor);
    void (*block)(const uint8_t *blk, size_t n);
    void (*end)(int id, uint32_t samples, const char *reason, int rms, int peak);
} audio_voice_cb_t;
void audio_voice_set(const audio_voice_cb_t *cb);
bool audio_has_mic(void);

/* For the screen: the live waveform of voice mode. Fills out[0..n) with heights 0..255 of the
 * last n columns (one column = 5 ms of filtered mic, oldest first) and returns a counter that
 * changes whenever new columns arrive. audio_hearing(): inside an utterance right now. */
uint32_t audio_wave(uint8_t *out, int n);
bool     audio_hearing(void);

bool   audio_stream_begin(void);
size_t audio_stream_write(const uint8_t *d, size_t n, uint32_t wait_ms);
void   audio_stream_end(void);

#ifdef __cplusplus
}
#endif
