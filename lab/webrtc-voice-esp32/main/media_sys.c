/*
 * media_sys.c — audio-only capture for WebRTC: the pet's ES8311 mic through esp_capture, which
 * encodes the Opus the peer negotiated. No camera, no player: the pet only sends.
 * (After esp-webrtc-solution doorbell_local/main/media_sys.c, Public Domain / CC0.)
 */
#include "common.h"
#include "esp_audio_enc_default.h"
#include "esp_capture_defaults.h"
#include "esp_log.h"
#include "mic.h"

#define TAG "MEDIA_SYS"

static esp_capture_handle_t s_capture;

int media_sys_buildup(void)
{
    esp_audio_enc_register_default();
    esp_codec_dev_handle_t rec = mic_open();
    if (!rec) return -1;
    esp_capture_audio_dev_src_cfg_t src_cfg = { .record_handle = rec };
    esp_capture_audio_src_if_t *aud_src = esp_capture_new_audio_dev_src(&src_cfg);
    if (!aud_src) { ESP_LOGE(TAG, "audio source failed"); return -1; }
    esp_capture_cfg_t cfg = { .sync_mode = ESP_CAPTURE_SYNC_MODE_AUDIO, .audio_src = aud_src };
    if (esp_capture_open(&cfg, &s_capture) != ESP_CAPTURE_ERR_OK) { ESP_LOGE(TAG, "capture open failed"); return -1; }
    ESP_LOGI(TAG, "capture ready: ES8311 mic");
    return 0;
}

int media_sys_get_provider(esp_webrtc_media_provider_t *provider)
{
    provider->capture = s_capture;
    provider->player = NULL;
    return 0;
}

int test_capture_to_player(void) { return -1; }
int play_music(const uint8_t *data, int size, int duration) { return -1; }
int stop_music(void) { return 0; }
