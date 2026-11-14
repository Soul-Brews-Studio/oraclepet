/*
 * jc3248-pet-idf — STEP 8: audio (the ribbit) — audio.c
 *
 * Ported from the Arduino body lab/jc3248-pet/src/audio.cpp. The DSP is VERBATIM:
 * tone()/syllable()/croak-via-syllable/gap()/softclip() are pure int16 PCM math
 * (Arduino.h swapped for math.h), and the master VOL/PEAK ceiling is unchanged.
 * ONLY the I2S transport was rewritten — from arduino-esp32's ESP_I2S I2SClass
 * to native ESP-IDF i2s_std (driver/i2s_std.h, component esp_driver_i2s):
 *
 *   i2s_new_channel(&chan_cfg, &s_tx, NULL)         // I2S_NUM_AUTO, ROLE_MASTER, TX only
 *   i2s_channel_init_std_mode(s_tx, &std_cfg)       // 16k clk + Philips 16-bit mono slot
 *   i2s_channel_enable(s_tx)
 *   i2s_channel_write(s_tx, buf, bytes, &wrote, portMAX_DELAY)   // blocking emit()
 *
 * Amp: NS4168 always-on (no enable GPIO). 16 kHz / 16-bit / mono.
 * Pins: BCK=GPIO42, WS/LRCK=GPIO2, DOUT=GPIO41 (ESP32 data-out → amp DIN), no MCLK.
 */
#include "audio.h"

#include <math.h>
#include <stdlib.h>
#include "esp_heap_caps.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/i2s_std.h"     /* I2S_STD_*_DEFAULT_CONFIG, i2s_channel_init_std_mode */
#include "driver/i2s_common.h"
#include "driver/gpio.h"  /* i2s_new_channel, i2s_channel_enable/write, I2S_GPIO_UNUSED */
#include "freertos/FreeRTOS.h"  /* portMAX_DELAY */
#include "freertos/task.h"      /* the async playback task */
#include "freertos/queue.h"       /* play-request queue */
#include "freertos/stream_buffer.h" /* the live stream buffer */
#include "esp_timer.h"
#include "esp_err.h"
#include "esp_log.h"
#include "board.h"
#if defined(PET_BOARD_LCD28)
#include "esp_codec_dev.h"           /* lcd28: the ES8311 codec, configured over I2C */
#include "esp_codec_dev_defaults.h"
#include "board_i2c.h"
#endif

static const char *TAG = "snd";

/* ---- I2S pin map (this board) — kept private to this TU ----
 * IMPORTANT: GPIO41 is the ESP32 DATA-OUT to the amp, so it goes in the .dout
 * slot of the gpio_cfg. Wiring it as .din = total silence. */
#if defined(PET_BOARD_LCD28)
/* lcd28: ES8311 codec (needs MCLK) + amp enable on GPIO1 (xiaozhi's freenove-2.8 map) */
#define PIN_MCLK  GPIO_NUM_4
#define PIN_BCK   GPIO_NUM_5
#define PIN_WS    GPIO_NUM_7
#define PIN_DOUT  GPIO_NUM_8
#define PIN_DIN   GPIO_NUM_6    /* the on-board mic, through the ES8311 ADC */
#define PIN_PA    1         /* amp enable, ACTIVE LOW on this board (xiaozhi passes pa_inverted = true) */
#elif defined(PET_BOARD_SC01)
/* sc01: NS4168 amp like the JC3248, on the WT32-SC01 Plus's I2S pins */
#define PIN_MCLK  I2S_GPIO_UNUSED
#define PIN_BCK   GPIO_NUM_36
#define PIN_WS    GPIO_NUM_35
#define PIN_DOUT  GPIO_NUM_37
#else
#define PIN_MCLK  I2S_GPIO_UNUSED   /* NS4168 needs no MCLK */
#define PIN_BCK   GPIO_NUM_42   /* BCLK            */
#define PIN_WS    GPIO_NUM_2    /* LRCK / word sel */
#define PIN_DOUT  GPIO_NUM_41   /* DIN on the amp = ESP32 data-out */
#endif

/* Output rate: 16 kHz mono on both boards, the clips' own rate. lcd28 ran the ES8311 at
 * 24 kHz (xiaozhi's rate) with clips upsampled 3:2 by averaging, which dulled the voice
 * ("not crisp"); the 24 kHz choice came from the wrong first guess about the silent speaker
 * (the cause was the amp polarity). At 16 kHz the codec clocks at MCLK 4.096 MHz. */
#define SR   16000u
#define CLIP_SR 16000u          /* PSND clips are always 16 kHz */
#define PI2  6.28318531f

/* MASTER VOLUME + HARD CEILING. Output power scales as VOL^2; the NS4168 browns
 * out a 500mA USB-2 budget above ~0.35 (0.50 ~576mA, 1.0 ~1.5A = "amp -> no boot").
 * 0.25 is the max-safe-on-USB with wide margin. For ear-splitting loud, run from
 * the onboard LiPo / a >=1A supply and raise BOTH constants. */
#define VOL_MAX    0.28f   /* 2026-10-21: 0.32 sagged the USB supply and garbled speech */
#define AUDIO_VOL  0.28f
static const float   VOL  = (AUDIO_VOL < VOL_MAX) ? AUDIO_VOL : VOL_MAX;
static const int16_t PEAK = (int16_t)(0x7FFF * ((AUDIO_VOL < VOL_MAX) ? AUDIO_VOL : VOL_MAX));

static i2s_chan_handle_t s_tx    = NULL;
static i2s_chan_handle_t s_rx    = NULL;          /* lcd28: mic, for the speaker self-test */
#if defined(PET_BOARD_LCD28)
static esp_codec_dev_handle_t s_dev = NULL;   /* the ES8311 as an esp_codec_dev output device */
#endif
static bool              s_ready = false;

/* Async playback: audio_ribbit()/audio_blip() post a request to s_q and return
 * IMMEDIATELY; the audio task plays it. This keeps the ~740ms ribbit OFF the main
 * render loop — a tap now shows the heart instantly (it used to wait for the whole
 * blocking sound to finish, which felt slower than the Arduino). */
typedef enum { SND_RIBBIT, SND_BLIP_UP, SND_BLIP_DOWN, SND_TONE, SND_CLIP, SND_SELFTEST, SND_STREAM, SND_LISTEN } snd_kind_t;
static int               s_listen_ms;
static bool              s_listen_send;
static audio_listen_cb_t s_listen_done;
static audio_chunk_cb_t  s_listen_chunk;
static audio_selftest_cb_t s_selftest_cb;
typedef struct { uint8_t kind; uint16_t hz, ms; char name[24]; } snd_cmd_t;

/* Speech/sound clips (STEP 13): IMA ADPCM files in /littlefs/sounds/<name>.ima, made
 * by OraclePet from macOS speech and sent by partial OTA. Clip gain per board: the
 * JC3248's NS4168 stays under its USB brownout ceiling; the ES8311 board has headroom. */
#if defined(PET_BOARD_LCD28)
#define CLIP_GAIN 0.85f     /* 1.0 with the codec at 100 % clipped: loud but unintelligible */
#else
#define CLIP_GAIN VOL
#endif
static QueueHandle_t     s_q = NULL;
static void audio_task(void *arg);   /* fwd decl (defined after audio_init) */

/* Blocking PCM emit — replaces I2SClass::write(buf, n*2). */
static inline void emit(const int16_t *b, uint32_t n)
{
#if defined(PET_BOARD_LCD28)
    static bool warned;
    int r = esp_codec_dev_write(s_dev, (void *)b, (int)n * 2);   /* through the codec device, as xiaozhi does */
    if (r != ESP_CODEC_DEV_OK && !warned) { warned = true; ESP_LOGE(TAG, "codec write failed: %d", r); }
#else
    size_t wrote = 0;
    i2s_channel_write(s_tx, (const void *)b, (size_t)n * 2, &wrote, portMAX_DELAY);
#endif
}

/* Soft clip adds grit (odd harmonics) and reads louder on a small speaker. */
static inline float softclip(float x)
{
    if (x >  1.0f) x =  1.0f;
    if (x < -1.0f) x = -1.0f;
    return 1.5f * x - 0.5f * x * x * x;
}

/* In-band single tone (the blips). ~5 ms attack/decay kills the click-pop. */
static void tone(float f, uint32_t ms)
{
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

/* One frog syllable: carrier glides f0->f1 (kept in the speaker's passband),
 * amplitude-modulated at modHz, with 2nd/3rd harmonics + soft clip — a buzzy
 * croak the tiny BTL driver can actually radiate. Phase-integrated for the glide. */
static void syllable(float f0, float f1, float modHz, uint32_t ms)
{
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

static void gap(uint32_t ms)
{
    if (!s_ready) return;
    const uint32_t n = SR * ms / 1000;
    int16_t z[256] = {0}; uint32_t left = n;
    while (left) { uint32_t c = left < 256 ? left : 256; emit(z, c); left -= c; }
}

void audio_init(void)
{
    if (s_ready) return;

    /* TX-only channel, master role. lcd28: I2S0 with auto-clear, as xiaozhi's ES8311 driver. */
#if defined(PET_BOARD_LCD28)
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear_after_cb = true;
#else
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    /* The DMA descriptors are a ring: without auto-clear the last ~90 ms of every sound
     * keep replaying after it ends (a faint ~11 Hz buzz). Found by the ES8311 audit agent. */
    chan_cfg.auto_clear_after_cb = true;
#endif
#if defined(PET_BOARD_LCD28)
    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx, &s_rx);   /* duplex, exactly as xiaozhi */
#else
    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx, NULL);
#endif
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
        return;
    }

    /* 16k Philips, 16-bit mono. GPIO41 in the .dout slot (ESP32 data-out → amp). */
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SR),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = PIN_MCLK,          /* NS4168: none; ES8311: 256 x fs */
            .bclk = PIN_BCK,
            .ws   = PIN_WS,
            .dout = PIN_DOUT,
#if defined(PET_BOARD_LCD28)
            .din  = PIN_DIN,
#else
            .din  = I2S_GPIO_UNUSED,   /* TX only */
#endif
            .invert_flags = {
                .mclk_inv = 0,
                .bclk_inv = 0,
                .ws_inv   = 0,
            },
        },
    };
    err = i2s_channel_init_std_mode(s_tx, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(err));
        i2s_del_channel(s_tx);
        s_tx = NULL;
        return;
    }

#if defined(PET_BOARD_LCD28)
    if (s_rx && (i2s_channel_init_std_mode(s_rx, &std_cfg) != ESP_OK || i2s_channel_enable(s_rx) != ESP_OK)) {
        ESP_LOGW(TAG, "mic channel failed; no speaker self-test");
        i2s_del_channel(s_rx);
        s_rx = NULL;
    }
#endif
    err = i2s_channel_enable(s_tx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable failed: %s", esp_err_to_name(err));
        i2s_del_channel(s_tx);
        s_tx = NULL;
        return;
    }

#if defined(PET_BOARD_LCD28)
    /* ES8311 bring-up, step for step as xiaozhi-esp32's Es8311AudioCodec (which plays on
     * this exact board), because the shortcut we had (codec_if set_fs + enable, raw
     * i2s writes, no reset hold) produced silence: the clip played to I2S in full
     * ("clip say: 46182 samples") and nothing came out. The reset hold is the part the
     * ES8311 init guide asks for: the codec does not reset with the ESP32, and its state
     * machine only starts when init releases a held reset. */
    {
        audio_codec_i2c_cfg_t i2c_cfg = { .port = BOARD_I2C_PORT, .addr = ES8311_CODEC_DEFAULT_ADDR,
                                          .bus_handle = board_i2c_bus() };
        const audio_codec_ctrl_if_t *ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg);
        const audio_codec_gpio_if_t *gpio = audio_codec_new_gpio();
        audio_codec_i2s_cfg_t i2s_cfg = { .port = I2S_NUM_0, .rx_handle = s_rx, .tx_handle = s_tx };
        const audio_codec_data_if_t *data = audio_codec_new_i2s_data(&i2s_cfg);
        if (!ctrl || !gpio || !data) { ESP_LOGE(TAG, "ES8311: codec interfaces failed"); return; }

        uint8_t v = 0x1F;                                  /* hold the digital blocks in reset */
        ctrl->write_reg(ctrl, 0x00, 1, &v, 1);
        vTaskDelay(pdMS_TO_TICKS(5));

        es8311_codec_cfg_t es = {
            .ctrl_if = ctrl, .gpio_if = gpio, .codec_mode = s_rx ? ESP_CODEC_DEV_WORK_MODE_BOTH : ESP_CODEC_DEV_WORK_MODE_DAC,
            .pa_pin = PIN_PA, .pa_reverted = true, .use_mclk = true,     /* amp on = GPIO1 low */
            .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
        };
        const audio_codec_if_t *codec = es8311_codec_new(&es);
        esp_codec_dev_cfg_t dev_cfg = { .dev_type = s_rx ? ESP_CODEC_DEV_TYPE_IN_OUT : ESP_CODEC_DEV_TYPE_OUT,
                                        .codec_if = codec, .data_if = data };
        s_dev = codec ? esp_codec_dev_new(&dev_cfg) : NULL;
        esp_codec_dev_sample_info_t fs = { .bits_per_sample = 16, .channel = 1, .sample_rate = SR, .mclk_multiple = 0 };
        if (!s_dev || esp_codec_dev_open(s_dev, &fs) != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "ES8311 open failed: sounds disabled");
            return;
        }
        esp_codec_dev_set_out_vol(s_dev, 90);              /* percent, through the codec's volume curve (100 distorted) */
        if (s_rx) esp_codec_dev_set_in_gain(s_dev, 30.0f);   /* mic, as xiaozhi */

        /* proof, not assumption: chip id (0x83 0x11) and the DAC path registers */
        uint8_t id1 = 0, id2 = 0, r00 = 0, r01 = 0, r0d = 0, r12 = 0, r14 = 0, r31 = 0, r32 = 0;
        ctrl->read_reg(ctrl, 0xFD, 1, &id1, 1);
        ctrl->read_reg(ctrl, 0xFE, 1, &id2, 1);
        ctrl->read_reg(ctrl, 0x00, 1, &r00, 1);
        ctrl->read_reg(ctrl, 0x01, 1, &r01, 1);
        ctrl->read_reg(ctrl, 0x0D, 1, &r0d, 1);
        ctrl->read_reg(ctrl, 0x12, 1, &r12, 1);
        ctrl->read_reg(ctrl, 0x14, 1, &r14, 1);
        ctrl->read_reg(ctrl, 0x31, 1, &r31, 1);
        ctrl->read_reg(ctrl, 0x32, 1, &r32, 1);
        ESP_LOGI(TAG, "ES8311 id %02X%02X  r00=%02X r01=%02X r0d=%02X r12=%02X r14=%02X r31=%02X r32=%02X  PA gpio%d",
                 id1, id2, r00, r01, r0d, r12, r14, r31, r32, PIN_PA);
        {   /* the whole register map, for comparing against a known-good dump */
            char line[3 * 0x46 + 1];
            int k = 0;
            for (int r = 0; r < 0x46; r++) {
                uint8_t x = 0;
                ctrl->read_reg(ctrl, r, 1, &x, 1);
                k += snprintf(line + k, sizeof line - k, "%02X ", x);
            }
            ESP_LOGI(TAG, "ES8311 regs 00..45: %s", line);
        }
        /* what the pins actually do: amp enable GPIO1 and the I2S lines */
        gpio_dump_io_configuration(stdout, (1ULL << 1) | (1ULL << 4) | (1ULL << 5) | (1ULL << 7) | (1ULL << 8));
    }
#endif

    s_ready = true;

    /* Playback task + request queue so audio never blocks the render loop. The DSP
     * uses small (256-sample) stack buffers + sinf, so 4KB is ample; prio 3 (below
     * the main loop) is fine — i2s_channel_write paces it. */
    s_q = xQueueCreate(4, sizeof(snd_cmd_t));
    if (!s_q || xTaskCreate(audio_task, "audio", 6144, NULL, 3, NULL) != pdPASS)
        ESP_LOGW(TAG, "audio task/queue create failed — sounds disabled");

    ESP_LOGI(TAG, "I2S TX ready (16k/16/mono BCK%d/WS%d/DOUT%d, vol=%d%% max=%d%%)",
             (int)PIN_BCK, (int)PIN_WS, (int)PIN_DOUT, (int)(VOL * 100), (int)(VOL_MAX * 100));
}

/* ---- IMA ADPCM clip (format: ../../pet-sdk/README.md "Sound clips") ----
 * file  = 16-byte header + blocks
 * header: "PSND", u8 version=1, u8 codec=1 (IMA ADPCM), u16 0, u32 sample_rate, u32 samples
 * block : int16 predictor, u8 step index, u8 0, then 252 bytes = 504 samples, low nibble first.
 * Every block restarts the decoder, so a bad block cannot corrupt the rest. */
#define CLIP_BLOCK      256
#define CLIP_BLOCK_SMP  504
static const int16_t ima_step[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767 };
static const int8_t ima_index[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };

/* PSND header -> sample count; false (and a log line) if it is not a 16 kHz IMA clip. */
static bool psnd_header(const uint8_t *h, const char *what, uint32_t *total)
{
    if (memcmp(h, "PSND", 4) != 0 || h[4] != 1 || h[5] != 1) {
        ESP_LOGW(TAG, "%s is not a PSND v1 IMA clip", what);
        return false;
    }
    uint32_t rate = h[8] | h[9] << 8 | h[10] << 16 | (uint32_t)h[11] << 24;
    *total = h[12] | h[13] << 8 | h[14] << 16 | (uint32_t)h[15] << 24;
    if (rate != CLIP_SR || *total > CLIP_SR * 30) {
        ESP_LOGW(TAG, "%s: %lu Hz, %lu samples: unsupported", what, (unsigned long)rate, (unsigned long)*total);
        return false;
    }
    return true;
}

/* Decode one 256-byte block (n samples) and play it (at SR; a 24 kHz build upsamples). */
static void play_block(const uint8_t *blk, uint32_t n, bool first)
{
    static int16_t out[CLIP_BLOCK_SMP];
    int pred = (int16_t)(blk[0] | blk[1] << 8);
    int idx = blk[2] > 88 ? 88 : blk[2];
    for (uint32_t i = 0; i < n; i++) {
        uint8_t nib = (blk[4 + i / 2] >> ((i & 1) * 4)) & 0x0F;
        int step = ima_step[idx], diff = step >> 3;
        if (nib & 1) diff += step >> 2;
        if (nib & 2) diff += step >> 1;
        if (nib & 4) diff += step;
        pred += (nib & 8) ? -diff : diff;
        if (pred > 32767) pred = 32767; else if (pred < -32768) pred = -32768;
        idx += ima_index[nib];
        if (idx < 0) idx = 0; else if (idx > 88) idx = 88;
        out[i] = (int16_t)((float)pred * CLIP_GAIN);
    }
#if SR == CLIP_SR
    (void)first;
    emit(out, n);
#else
    {   /* 16 -> 24 kHz: every 2 input samples become 3 (linear), carrying the last sample */
        static int16_t up[CLIP_BLOCK_SMP * 3 / 2 + 3];
        static int16_t prev = 0;
        if (first) prev = 0;
        uint32_t m = 0;
        for (uint32_t i = 0; i + 1 < n; i += 2) {
            int a = i ? out[i - 1] : prev, b = out[i], c = out[i + 1];
            up[m++] = (int16_t)((a + 2 * b) / 3);
            up[m++] = (int16_t)((2 * b + c) / 3);
            up[m++] = (int16_t)c;
        }
        if (n & 1) up[m++] = out[n - 1];
        prev = out[n - 1];
        emit(up, m);
    }
#endif
}

static void play_clip(const char *name)
{
    if (!s_ready) return;
    char path[64];
    snprintf(path, sizeof path, "/littlefs/sounds/%.23s.ima", name);
    FILE *f = fopen(path, "rb");
    if (!f) { ESP_LOGW(TAG, "no clip %s", path); return; }
    uint8_t h[16];
    uint32_t total = 0;
    if (fread(h, 1, 16, f) != 16 || !psnd_header(h, path, &total)) { fclose(f); return; }
    static uint8_t blk[CLIP_BLOCK];
    uint32_t done = 0;
    while (done < total && fread(blk, 1, CLIP_BLOCK, f) >= 4) {
        uint32_t n = total - done < CLIP_BLOCK_SMP ? total - done : CLIP_BLOCK_SMP;
        play_block(blk, n, done == 0);
        done += n;
    }
    fclose(f);
    ESP_LOGI(TAG, "clip %s: %lu samples (%.1f s)", name, (unsigned long)done, done / (float)CLIP_SR);
}

/* ---- live stream (STEP 13b): a PSND clip played WHILE it arrives over BLE ----
 * ota.c feeds the bytes in with audio_stream_write() (it blocks when this buffer is
 * full, which slows the BLE acks and so the sender: no overflow, no drop); the audio
 * task plays each 256-byte block as soon as it is complete. */
#define STREAM_BUF  (16 * 1024)             /* ~2 s of 16 kHz IMA ADPCM (8 KB ran dry near the end of long lines) */
#define STREAM_PREBUF (24 * CLIP_BLOCK)   /* ~0.75 s buffered before the first sound */
static StreamBufferHandle_t s_stream;
static volatile bool        s_stream_end;

/* read exactly n bytes, or fewer at the end of the stream (false = stalled/ended) */
static size_t stream_read(uint8_t *dst, size_t n)
{
    size_t got = 0;
    int idle = 0;
    while (got < n) {
        size_t r = xStreamBufferReceive(s_stream, dst + got, n - got, pdMS_TO_TICKS(100));
        got += r;
        if (r) { idle = 0; continue; }
        if (s_stream_end && xStreamBufferIsEmpty(s_stream)) break;
        if (++idle > 50) { ESP_LOGW(TAG, "stream stalled"); break; }   /* 5 s without data */
    }
    return got;
}

static int64_t s_stream_t0;               /* when the sender began the stream */

static void play_stream(void)
{
    int64_t t0 = s_stream_t0;
    uint8_t h[16];
    uint32_t total = 0;
    if (stream_read(h, 16) != 16 || !psnd_header(h, "stream", &total)) return;
    /* A cushion before the first sound: Bluetooth delivers in bursts, and playing each block
     * the moment it lands made the voice stutter whenever the link paused (two pets on one
     * radio, a heartbeat in between). Wait for ~0.75 s of audio, the whole clip if it is
     * shorter, or 1.2 s at most. */
    size_t want = (size_t)((total + CLIP_BLOCK_SMP - 1) / CLIP_BLOCK_SMP) * CLIP_BLOCK;
    if (want > STREAM_PREBUF) want = STREAM_PREBUF;
    for (int i = 0; i < 60 && !s_stream_end && xStreamBufferBytesAvailable(s_stream) < want; i++)
        vTaskDelay(pdMS_TO_TICKS(20));
    static uint8_t blk[CLIP_BLOCK];
    uint32_t done = 0;
    int64_t first_us = 0;
    while (done < total) {
        size_t got = stream_read(blk, CLIP_BLOCK);
        if (got < 4) break;
        uint32_t n = total - done < CLIP_BLOCK_SMP ? total - done : CLIP_BLOCK_SMP;
        if ((got - 4) * 2 < n) n = (uint32_t)(got - 4) * 2;     /* a short last block */
        if (!first_us) first_us = esp_timer_get_time();
        play_block(blk, n, done == 0);
        done += n;
    }
    ESP_LOGI(TAG, "stream: %lu samples (%.1f s), first sound %lld ms after the stream began",
             (unsigned long)done, done / (float)CLIP_SR, first_us ? (first_us - t0) / 1000 : -1LL);
    xStreamBufferReset(s_stream);
}

/* ---- actual (blocking) playback — runs ONLY on the audio task, never the loop ---- */
static void play_blip(bool up)
{
    tone(up ? 1000.0f : 750.0f, 80);   // in-band, distinct higher/lower
}

static void play_ribbit(void)
{
    // "rib-bit" = high syllable + lower syllable; repeated 3x for a longer call
    // (~740 ms total). All carriers stay in the speaker's passband so it's audible.
    for (int r = 0; r < 3; r++) {
        syllable(520, 360, 55, 70);   // "rib"
        gap(22);
        syllable(430, 300, 52, 95);   // "bit"
        if (r < 2) gap(90);           // pause between calls
    }
}

#if defined(PET_BOARD_LCD28)
/* Speaker self-test (lcd28 has a mic on the same codec): the pet hears itself. Room
 * noise first, then a 1 kHz tone while recording; a Goertzel filter at 1 kHz compares
 * the two. Objective evidence of sound, instead of asking someone to listen. */
static float goertzel_1k(const int16_t *x, int n)
{
    const float w = PI2 * 1000.0f / (float)SR, c = 2.0f * cosf(w);
    float s1 = 0, s2 = 0;
    for (int i = 0; i < n; i++) { float s0 = x[i] + c * s1 - s2; s2 = s1; s1 = s0; }
    return s1 * s1 + s2 * s2 - c * s1 * s2;
}

static void speaker_selftest(void)
{
    if (!s_rx || !s_dev) { if (s_selftest_cb) s_selftest_cb(false, false, 0, 0, 0); return; }
    enum { N = 256 };
    static int16_t tx[N], rx[N];
    const int per_s = SR / N;                              /* blocks per second */
    float q1k = 0, t1k = 0;
    double qe = 0, te = 0;
    int qn = 0, tn = 0;
    memset(tx, 0, sizeof tx);
    for (int k = 0; k < per_s; k++) {                      /* 1 s: 0.6 s warm-up (ADC start-up), 0.4 s quiet */
        emit(tx, N);
        if (esp_codec_dev_read(s_dev, rx, sizeof rx) != ESP_CODEC_DEV_OK) { ESP_LOGW(TAG, "self-test: mic read failed"); return; }
        if (k >= per_s * 6 / 10) { q1k += goertzel_1k(rx, N); for (int i = 0; i < N; i++) qe += (double)rx[i] * rx[i]; qn++; }
    }
    uint32_t ph = 0;
    for (int k = 0; k < per_s; k++) {                      /* 1 s of 1 kHz at half scale; skip the first 0.2 s */
        for (int i = 0; i < N; i++, ph++) tx[i] = (int16_t)(16000 * sinf(PI2 * 1000.0f * (float)ph / (float)SR));
        emit(tx, N);
        if (esp_codec_dev_read(s_dev, rx, sizeof rx) != ESP_CODEC_DEV_OK) { ESP_LOGW(TAG, "self-test: mic read failed"); return; }
        if (k >= per_s / 5) { t1k += goertzel_1k(rx, N); for (int i = 0; i < N; i++) te += (double)rx[i] * rx[i]; tn++; }
    }
    memset(tx, 0, sizeof tx);
    emit(tx, N);
    double qr = sqrt(qe / ((double)qn * N)), tr = sqrt(te / ((double)tn * N));
    float db = 10.0f * log10f((t1k / tn + 1.0f) / (q1k / qn + 1.0f));
    bool heard = db > 15.0f && tr > 3.0 * qr + 20.0;      /* both: the tone's frequency AND a louder mic */
    ESP_LOGI(TAG, "speaker self-test @%u Hz: mic rms quiet %.0f, tone %.0f; 1 kHz %+.1f dB -> %s",
             (unsigned)SR, qr, tr, db, heard ? "AUDIBLE" : "SILENT");
    if (s_selftest_cb) s_selftest_cb(true, heard, db, (int)qr, (int)tr);
}
#endif

/* ---- LISTEN: the board's mic (lcd28: ES8311 ADC) ----
 * Record at the codec rate, downsample to 16 kHz, measure, optionally send the recording
 * back as a PSND IMA stream (encoder = PROTOCOL.md section 5, bit-exact with the decoder). */
static void ima_encode_block(const int16_t *pcm, uint32_t n, int *pred, int *idx, uint8_t *blk)
{
    memset(blk, 0, CLIP_BLOCK);
    int16_t p16 = (int16_t)*pred;
    blk[0] = (uint8_t)(p16 & 0xFF);
    blk[1] = (uint8_t)((uint16_t)p16 >> 8);
    blk[2] = (uint8_t)*idx;
    for (uint32_t k = 0; k < n; k++) {
        int diff = pcm[k] - *pred, nib = 0;
        if (diff < 0) { nib = 8; diff = -diff; }
        int t = ima_step[*idx];
        if (diff >= t) { nib |= 4; diff -= t; }
        t >>= 1;
        if (diff >= t) { nib |= 2; diff -= t; }
        t >>= 1;
        if (diff >= t) { nib |= 1; }
        int step = ima_step[*idx], d = step >> 3;                /* the decoder step */
        if (nib & 1) d += step >> 2;
        if (nib & 2) d += step >> 1;
        if (nib & 4) d += step;
        *pred += (nib & 8) ? -d : d;
        if (*pred > 32767) *pred = 32767; else if (*pred < -32768) *pred = -32768;
        *idx += ima_index[nib];
        if (*idx < 0) *idx = 0; else if (*idx > 88) *idx = 88;
        blk[4 + k / 2] |= (uint8_t)(nib << ((k & 1) * 4));
    }
}

static int cmp_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

static void do_listen(void)
{
    audio_listen_result_t r = { .ms = s_listen_ms };
#if !defined(PET_BOARD_LCD28)
    if (s_listen_done) s_listen_done(&r);                    /* no mic on this board */
    return;
#else
    if (!s_rx || !s_dev) { if (s_listen_done) s_listen_done(&r); return; }
    r.has_mic = true;
    const uint32_t want = CLIP_SR * (uint32_t)s_listen_ms / 1000;
    int16_t *pcm = heap_caps_malloc(want * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pcm) { r.has_mic = false; if (s_listen_done) s_listen_done(&r); return; }
    /* record at SR (24 kHz) and keep 2 of every 3 samples' worth: a, (b+c)/2 -> 16 kHz */
    enum { IN = 240 };                                        /* 10 ms at 24 kHz */
    static int16_t in[IN];
    uint32_t got = 0;
    while (got < want) {
        if (esp_codec_dev_read(s_dev, in, sizeof in) != ESP_CODEC_DEV_OK) break;
#if SR == CLIP_SR
        for (int i = 0; i < IN && got < want; i++) pcm[got++] = in[i];
#else
        for (int i = 0; i + 2 < IN && got < want; i += 3) {
            pcm[got++] = in[i];
            if (got < want) pcm[got++] = (int16_t)((in[i + 1] + in[i + 2]) / 2);
        }
#endif
    }
    r.samples = got;
    /* level: RMS, peak, and the floor = the quietest 100 ms */
    double e = 0, wmin = -1;
    int peak = 0;
    for (uint32_t w = 0; w + 1600 <= got; w += 1600) {
        double we = 0;
        for (uint32_t i = w; i < w + 1600; i++) we += (double)pcm[i] * pcm[i];
        if (wmin < 0 || we < wmin) wmin = we;
    }
    for (uint32_t i = 0; i < got; i++) {
        e += (double)pcm[i] * pcm[i];
        int a = pcm[i] < 0 ? -pcm[i] : pcm[i];
        if (a > peak) peak = a;
    }
    r.rms = got ? (int)sqrt(e / got) : 0;
    r.peak = peak;
    r.floor = wmin >= 0 ? (int)sqrt(wmin / 1600) : r.rms;
    /* strongest frequency 150-4000 Hz (Goertzel every 50 Hz) and how far it stands out */
    enum { BINS = (4000 - 150) / 50 + 1 };
    static float pw[BINS], sorted[BINS];
    for (int b = 0; b < BINS; b++) {
        const float w = PI2 * (150 + 50 * b) / (float)CLIP_SR, c = 2.0f * cosf(w);
        float s1 = 0, s2 = 0;
        for (uint32_t i = 0; i < got; i++) { float s0 = pcm[i] + c * s1 - s2; s2 = s1; s1 = s0; }
        pw[b] = (s1 * s1 + s2 * s2 - c * s1 * s2) / (got ? got : 1);
        sorted[b] = pw[b];
    }
    int best = 0;
    for (int b = 1; b < BINS; b++) if (pw[b] > pw[best]) best = b;
    qsort(sorted, BINS, sizeof(float), cmp_float);
    r.freq = 150 + 50 * best;
    r.db = 10.0f * log10f((pw[best] + 1.0f) / (sorted[BINS / 2] + 1.0f));
    ESP_LOGI(TAG, "listen %d ms: rms %d peak %d floor %d, strongest %d Hz %+.1f dB", r.ms, r.rms, r.peak, r.floor, r.freq, r.db);
    /* the recording itself, as a PSND stream */
    if (s_listen_send && s_listen_chunk && got) {
        uint8_t h[16] = { 'P', 'S', 'N', 'D', 1, 1, 0, 0 };
        h[8] = CLIP_SR & 0xFF; h[9] = (CLIP_SR >> 8) & 0xFF; h[10] = (CLIP_SR >> 16) & 0xFF; h[11] = 0;
        h[12] = got & 0xFF; h[13] = (got >> 8) & 0xFF; h[14] = (got >> 16) & 0xFF; h[15] = (got >> 24) & 0xFF;
        bool ok = s_listen_chunk(h, sizeof h);
        static uint8_t blk[CLIP_BLOCK];
        int pred = 0, idx = 0;
        for (uint32_t i = 0; ok && i < got; i += CLIP_BLOCK_SMP) {
            uint32_t n = got - i < CLIP_BLOCK_SMP ? got - i : CLIP_BLOCK_SMP;
            ima_encode_block(pcm + i, n, &pred, &idx, blk);
            ok = s_listen_chunk(blk, CLIP_BLOCK);
        }
        r.sent = ok;
    }
    free(pcm);
    if (s_listen_done) s_listen_done(&r);
#endif
}

/* ---- voice mode: the mic, always on, cut into utterances (VOICE.md §3) ---- */
static audio_voice_cb_t s_voice;
static volatile bool    s_voice_on;

void audio_voice_set(const audio_voice_cb_t *cb)
{
    if (cb) s_voice = *cb;
    s_voice_on = cb != NULL;
    ESP_LOGI(TAG, "voice mode %s", cb ? "on" : "off");
}

/* the waveform for the screen: one peak per 5 ms column, written by the audio task, read by the
 * main loop (a torn read only misdraws one column for one frame) */
enum { WAVE_N = 256, WAVE_COL = 80 };
static uint8_t           s_wave[WAVE_N];
static volatile uint32_t s_wave_seq;
static volatile bool     s_hearing;

uint32_t audio_wave(uint8_t *out, int n)
{
    if (n > WAVE_N) n = WAVE_N;
    uint32_t seq = s_wave_seq;
    for (int i = 0; i < n; i++) out[i] = s_wave[(seq - n + i) % WAVE_N];
    return seq;
}

bool audio_hearing(void) { return s_hearing; }

bool audio_has_mic(void)
{
#if defined(PET_BOARD_LCD28)
    return s_rx && s_dev;
#else
    return false;
#endif
}

#if defined(PET_BOARD_LCD28)
/* 20 ms frames. Re-tuned 2026-10-21 on a Meet call heard from the Mac's speakers (floor ~10, speech
 * RMS 150-300): the first gate (start and hold at floor + 120, end after 800 ms) sent 62% of the
 * call, in pieces 0.8 s apart, and lost the soft syllables between them. */
enum { VF = 320, PRE = 15, HANG = 60, HOLD_MIN = 3, MIN_SPEECH = 13, MAX_SMP = 30 * 16000, FLOORWIN = 150 };
static struct {
    float    floor;
    float    mins[FLOORWIN];                      /* frame RMS of the last 3 s: the floor is their minimum */
    int      mins_i;
    uint8_t  hist[HANG];                          /* the last 1.2 s: which frames sounded like voice */
    int      hist_i, hist_n;
    float    r1, r2;                              /* the two frames before this one */
    int      above, quiet, speech, id;
    bool     on;                                  /* inside an utterance */
    int16_t  ring[PRE][VF];                       /* the 300 ms before speech starts */
    int      ring_n, ring_head;
    int16_t  pend[CLIP_BLOCK_SMP];                /* samples waiting for a whole block */
    int      pend_n, pred, idx, peak;
    uint32_t samples;
    double   energy;
} v;

static void voice_emit(const int16_t *s, int n)  /* utterance audio -> blocks */
{
    for (int i = 0; i < n; i++) {
        int a = s[i] < 0 ? -s[i] : s[i];
        if (a > v.peak) v.peak = a;
        v.energy += (double)s[i] * s[i];
        v.pend[v.pend_n++] = s[i];
        if (v.pend_n == CLIP_BLOCK_SMP) {
            static uint8_t blk[CLIP_BLOCK];
            ima_encode_block(v.pend, CLIP_BLOCK_SMP, &v.pred, &v.idx, blk);
            if (s_voice.block) s_voice.block(blk, CLIP_BLOCK);
            v.pend_n = 0;
        }
    }
    v.samples += n;
}

static void voice_finish(const char *reason)
{
    if (!v.on) return;
    if (v.pend_n) {                               /* the partial last block, zero-padded */
        static uint8_t blk[CLIP_BLOCK];
        ima_encode_block(v.pend, v.pend_n, &v.pred, &v.idx, blk);
        if (s_voice.block) s_voice.block(blk, CLIP_BLOCK);
        v.pend_n = 0;
    }
    const char *why = (v.speech < MIN_SPEECH && strcmp(reason, "silence") == 0) ? "short" : reason;
    int rms = v.samples ? (int)sqrt(v.energy / v.samples) : 0;
    if (s_voice.end) s_voice.end(v.id, v.samples, why, rms, v.peak);
    ESP_LOGI(TAG, "voice #%d: %.1f s, %s (rms %d, floor %d)", v.id, v.samples / 16000.0f, why, rms, (int)v.floor);
    v.on = false;
    s_hearing = false;
}

static void voice_frame(void)
{
    static int16_t f[VF];
    static int reads, fails, maxrms;
    int rc = esp_codec_dev_read(s_dev, f, sizeof f);
    if (rc != ESP_CODEC_DEV_OK) { fails++; vTaskDelay(pdMS_TO_TICKS(20)); }
    /* 2nd-order high-pass at 200 Hz (RBJ, Q 0.707) before the VAD and the encoder: speech lives
     * above it; below it was the WiFi rumble (50 and 125 Hz, 80% of the noise). */
    static const float b0 = 0.945976f, b1 = -1.891953f, b2 = 0.945976f, a1 = -1.889032f, a2 = 0.894873f;
    static float x1, x2, y1, y2;
    double e = 0;
    for (int i = 0; rc == ESP_CODEC_DEV_OK && i < VF; i++) {
        float x = f[i], y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x; y2 = y1; y1 = y;
        f[i] = (int16_t)(y > 32767 ? 32767 : y < -32768 ? -32768 : y);
        e += (double)f[i] * f[i];
    }
    float rms = (float)sqrt(e / VF);
    if (rc == ESP_CODEC_DEV_OK) {                 /* the screen's waveform: peak per 5 ms, square-root scaled */
        for (int c = 0; c < VF; c += WAVE_COL) {
            int pk = 0;
            for (int i = c; i < c + WAVE_COL; i++) { int a = f[i] < 0 ? -f[i] : f[i]; if (a > pk) pk = a; }
            float h = sqrtf(fminf(pk / 8000.0f, 1.0f)) * 255.0f;
            s_wave[s_wave_seq % WAVE_N] = (uint8_t)h;
            s_wave_seq++;
        }
    }
    if (rms > maxrms) maxrms = (int)rms;
    if (++reads % 100 == 0) {                     /* every 2 s: what the VAD sees */
        ESP_LOGI(TAG, "vad: %d frames, %d failed reads, rms max %d, floor %d, %s", reads, fails, maxrms,
                 (int)v.floor, v.on ? "in an utterance" : "waiting");
        maxrms = 0;
    }
    if (rc != ESP_CODEC_DEV_OK) return;
    /* the floor: the quietest frame of the last 3 s, always (also inside an utterance, so a
     * pause between words is measured against the real noise, and noise bursts cannot raise it;
     * 3 s and not 1.5 s, so fast talk without a real pause cannot lift it to the speech level) */
    v.mins[v.mins_i] = rms;
    v.mins_i = (v.mins_i + 1) % FLOORWIN;
    float lo = 1e9f;
    for (int k = 0; k < FLOORWIN; k++) if (v.mins[k] > 0 && v.mins[k] < lo) lo = v.mins[k];
    v.floor = lo < 1e9f ? fmaxf(lo, 1.0f) : fmaxf(rms, 1.0f);
    /* Two thresholds (hysteresis). The server's Silero VAD decides what is speech (for free), so
     * the pet errs towards sending a little noise rather than losing a quiet word.
     * Start: this frame AND the two before it above 2x the floor (+60). Measured on the ES3C28P,
     * WiFi's noise comes as single 20 ms bursts (17 in a quiet 5 s, run length 1), while a
     * syllable lasts 60 ms or more: one burst cannot start an utterance.
     * Hold: two frames in a row above 1.5x the floor (+30), so soft syllables and word ends keep
     * it open, and a single burst still cannot. */
    float on_th = fmaxf(2 * v.floor, v.floor + 60), hold_th = fmaxf(1.5f * v.floor, v.floor + 30);
    bool speechy = fminf(rms, fminf(v.r1, v.r2)) > on_th;
    bool voiced = fminf(rms, v.r1) > hold_th;
    v.r2 = v.r1;
    v.r1 = rms;
    if (!v.on) {
        v.above = speechy ? v.above + 1 : 0;
        memcpy(v.ring[v.ring_head], f, sizeof f);
        v.ring_head = (v.ring_head + 1) % PRE;
        if (v.ring_n < PRE) v.ring_n++;
        if (v.above >= 1) {                       /* 3 loud frames in a row (60 ms): an utterance begins */
            v.on = true; v.id++; v.quiet = 0; v.speech = v.above; v.above = 0;
            s_hearing = true;
            memset(v.hist, 1, sizeof v.hist); v.hist_n = HANG; v.hist_i = 0;   /* a fresh hangover */
            v.samples = 0; v.peak = 0; v.energy = 0; v.pred = 0; v.idx = 0; v.pend_n = 0;
            if (s_voice.start) s_voice.start(v.id, (int)v.floor);
            for (int k = 0; k < v.ring_n; k++)    /* oldest first: pre-roll + the frames that started it */
                voice_emit(v.ring[(v.ring_head - v.ring_n + k + PRE) % PRE], VF);
            v.ring_n = 0;
        }
        return;
    }
    voice_emit(f, VF);
    /* the end: fewer than 3 voiced frames in the last 1.2 s (a pause between sentences, not
     * between words). Counting in a window, not "60 quiet frames in a row", so a noise spike
     * does not keep an utterance open for ever. */
    v.speech += voiced;
    v.hist[v.hist_i] = voiced;
    v.hist_i = (v.hist_i + 1) % HANG;
    int recent = 0;
    for (int k = 0; k < HANG; k++) recent += v.hist[k];
    if (recent < HOLD_MIN) voice_finish("silence");
    else if (v.samples >= MAX_SMP) voice_finish("max");
}
#endif

#if defined(PET_BOARD_LCD28)
static bool s_pa_off;                            /* voice mode switched the amp off to listen */
#endif

static void audio_task(void *arg)
{

    (void)arg;
    snd_cmd_t cmd;
    for (;;) {
#if defined(PET_BOARD_LCD28)
        if (s_voice_on && s_rx && s_dev) {           /* voice mode: listen whenever nothing plays */
            if (xQueueReceive(s_q, &cmd, 0) != pdTRUE) {
                /* the speaker amp off while listening: an idle class-D amp next to the mic only adds
                 * noise (Codex review). On again just before the pet makes a sound. */
                if (!s_pa_off) { esp_codec_dev_set_out_mute(s_dev, true); gpio_set_level(PIN_PA, 1); s_pa_off = true; }
                voice_frame();
                continue;
            }
            voice_finish("stop");                    /* the pet is about to speak: close the utterance */
            if (s_pa_off) {                          /* amp on (active low on this board) */
                esp_codec_dev_set_out_mute(s_dev, false);
                gpio_set_level(PIN_PA, 0);
                s_pa_off = false;
            }
        } else {
            voice_finish("stop");
            /* wake every 100 ms: voice mode is switched on from another task and must not wait
             * for the next sound to be noticed */
            if (xQueueReceive(s_q, &cmd, pdMS_TO_TICKS(100)) != pdTRUE) continue;
        }
        {
#else
        if (xQueueReceive(s_q, &cmd, portMAX_DELAY) == pdTRUE) {
#endif
            switch (cmd.kind) {
                case SND_RIBBIT:    play_ribbit();     break;
                case SND_BLIP_UP:   play_blip(true);   break;
                case SND_BLIP_DOWN: play_blip(false);  break;
                case SND_TONE:      tone((float)cmd.hz, cmd.ms); break;
                case SND_CLIP:      play_clip(cmd.name); break;
                case SND_STREAM:    play_stream();       break;
                case SND_LISTEN:    do_listen();         break;
                case SND_SELFTEST:
#if defined(PET_BOARD_LCD28)
                    speaker_selftest();
#else
                    tone(1000.0f, 600);                      /* no mic on this board: just beep */
                    if (s_selftest_cb) s_selftest_cb(false, false, 0, 0, 0);
#endif
                    break;
            }
        }
    }
}

/* ---- public API: post a request and return immediately (drop if queue full) ---- */
void audio_blip(bool up)
{
    if (!s_q) return;
    snd_cmd_t c = { .kind = up ? SND_BLIP_UP : SND_BLIP_DOWN };
    xQueueSend(s_q, &c, 0);
}

void audio_tone(int hz, int ms)
{
    if (!s_q) return;
    if (hz < 50) hz = 50; else if (hz > 8000) hz = 8000;      /* below Nyquist at 16 kHz */
    if (ms < 10) ms = 10; else if (ms > 2000) ms = 2000;
    snd_cmd_t c = { .kind = SND_TONE, .hz = (uint16_t)hz, .ms = (uint16_t)ms };
    xQueueSend(s_q, &c, 0);
}

bool audio_listen(int ms, bool send, audio_listen_cb_t done, audio_chunk_cb_t chunk)
{
    if (!s_q) return false;
    s_listen_ms = ms;
    s_listen_send = send;
    s_listen_done = done;
    s_listen_chunk = chunk;
    snd_cmd_t c = { .kind = SND_LISTEN };
    return xQueueSend(s_q, &c, 0) == pdTRUE;
}

bool audio_stream_begin(void)
{
    if (!s_q) return false;
    /* PSRAM: with WiFi up there are not 16 KB of internal RAM left, and a failed create came
     * back to the host as "audio busy" */
    if (!s_stream) s_stream = xStreamBufferCreateWithCaps(STREAM_BUF, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_stream) return false;
    xStreamBufferReset(s_stream);
    s_stream_end = false;
    s_stream_t0 = esp_timer_get_time();
    snd_cmd_t c = { .kind = SND_STREAM };
    return xQueueSend(s_q, &c, 0) == pdTRUE;
}

size_t audio_stream_write(const uint8_t *d, size_t n, uint32_t wait_ms)
{
    if (!s_stream) return 0;
    return xStreamBufferSend(s_stream, d, n, pdMS_TO_TICKS(wait_ms));
}

void audio_stream_end(void) { s_stream_end = true; }

bool audio_selftest(audio_selftest_cb_t done)
{
    if (!s_q) return false;
    s_selftest_cb = done;
    snd_cmd_t c = { .kind = SND_SELFTEST };
    return xQueueSend(s_q, &c, 0) == pdTRUE;
}

bool audio_play(const char *name)
{
    if (!s_q || !name || !name[0]) return false;
    snd_cmd_t c = { .kind = SND_CLIP };
    snprintf(c.name, sizeof c.name, "%s", name);
    return xQueueSend(s_q, &c, 0) == pdTRUE;
}

void audio_ribbit(void)
{
    if (!s_q) return;
    snd_cmd_t c = { .kind = SND_RIBBIT };
    xQueueSend(s_q, &c, 0);
}
