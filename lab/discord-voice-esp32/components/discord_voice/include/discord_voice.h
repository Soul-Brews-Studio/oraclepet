/*
 * discord_voice.h — an ESP32 as its own Discord voice bot (lab/discord-voice-esp32/README.md).
 *
 * dv_start() logs the bot in on the Discord gateway, asks to join one voice channel, opens the
 * voice gateway (v8) and the UDP media path, and keeps all of it alive on PSRAM-stack tasks.
 * The caller then feeds 20 ms of 16 kHz mono PCM at a time with dv_send_pcm(); the component
 * encodes Opus, encrypts (AES-256-GCM, rtpsize) and sends RTP. No DAVE: it identifies with
 * max_dave_protocol_version 0 and reports DV_EVT_DAVE_REQUIRED if Discord asks for E2EE.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DV_FRAME_SAMPLES 320          /* 20 ms at 16 kHz */

typedef enum {
    DV_EVT_GATEWAY_READY,             /* logged in; text = the bot's user id */
    DV_EVT_VOICE_READY,               /* in the channel, keys agreed: dv_send_pcm() is heard */
    DV_EVT_DAVE_REQUIRED,             /* Discord asked for DAVE (E2EE); this build cannot */
    DV_EVT_ERROR,                     /* text = why; the component retries on its own */
    DV_EVT_DISCONNECTED,
} dv_event_t;

typedef void (*dv_event_cb)(dv_event_t evt, const char *text);

typedef struct {
    const char *token;                /* bot token (keep it out of git) */
    const char *guild_id;
    const char *channel_id;           /* the voice channel to join */
    dv_event_cb on_event;             /* may be NULL */
} dv_config_t;

/* Start (connects in the background). false: out of memory or bad config. */
bool dv_start(const dv_config_t *cfg);

/* 20 ms of 16 kHz mono PCM (DV_FRAME_SAMPLES samples). Encoded and sent if the voice link is
 * ready; dropped otherwise. Call it every 20 ms while speaking. */
void dv_send_pcm(const int16_t *pcm);

/* Speaking off: five Opus silence frames, then the speaking flag is cleared. */
void dv_silence(void);

bool dv_voice_ready(void);
const char *dv_state(void);           /* "gateway", "joining", "voice", "ready", "error: …" */

#ifdef __cplusplus
}
#endif
