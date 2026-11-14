/*
 * webrtc.c — the pet's voice over WebRTC: one peer at a time, Opus 16 kHz mono from the ES8311
 * (send only), plus a data channel for text. The pet is the HTTPS signaling server
 * (webrtc_http_server.c, from esp-webrtc-solution doorbell_local): the browser page sends
 * "LISTEN" over signaling, the pet makes the offer, the page answers.
 */
#include <string.h>

#include "common.h"
#include "esp_log.h"
#include "esp_peer_default.h"
#include "esp_timer.h"
#include "media_lib_os.h"
#include "webrtc_http_server.h"

#define TAG "PET_RTC"

static esp_webrtc_handle_t webrtc;
static uint16_t s_channel = 0xFFFF;               /* the open data channel's stream id */
static volatile bool s_connected;
static volatile bool s_active;                   /* a call is connecting or up: ignore another LISTEN */

void pet_say(const char *text)
{
    if (!webrtc || s_channel == 0xFFFF || !text) return;
    esp_peer_handle_t peer = NULL;
    esp_webrtc_get_peer_connection(webrtc, &peer);
    if (!peer) return;
    esp_peer_data_frame_t f = {
        .type = ESP_PEER_DATA_CHANNEL_STRING,
        .stream_id = s_channel,
        .data = (uint8_t *)text,
        .size = strlen(text),
    };
    esp_peer_send_data(peer, &f);
}

static int on_cmd(esp_webrtc_custom_data_via_t via, uint8_t *data, int size, void *ctx)
{
    if (via != ESP_WEBRTC_CUSTOM_DATA_VIA_SIGNALING || size == 0 || !webrtc) return 0;
    ESP_LOGI(TAG, "command %.*s", size, (char *)data);
    if (size == 6 && memcmp(data, "LISTEN", 6) == 0) {
        /* a repeated POST (the browser retries it on a busy socket) must not restart a live call */
        if (s_active) { ESP_LOGI(TAG, "already in a call: LISTEN ignored"); return 0; }
        s_active = true;
        esp_webrtc_enable_peer_connection(webrtc, true);
    } else if (size == 4 && memcmp(data, "STOP", 4) == 0) {
        s_active = false;
        esp_webrtc_enable_peer_connection(webrtc, false);
    }
    return 0;
}

static int on_channel_open(esp_peer_data_channel_info_t *ch, void *ctx)
{
    ESP_LOGI(TAG, "data channel '%s' open (stream %d)", ch->label ? ch->label : "?", ch->stream_id);
    s_channel = ch->stream_id;
    return 0;
}

static int on_channel_close(esp_peer_data_channel_info_t *ch, void *ctx)
{
    if (ch->stream_id == s_channel) s_channel = 0xFFFF;
    return 0;
}

static int on_data(esp_peer_data_frame_t *frame, void *ctx)
{
    ESP_LOGI(TAG, "browser says: %.*s", frame->size, (char *)frame->data);
    char reply[96];
    snprintf(reply, sizeof reply, "pet heard: %.*s", frame->size > 60 ? 60 : frame->size, (char *)frame->data);
    pet_say(reply);
    return 0;
}

static int on_event(esp_webrtc_event_t *event, void *ctx)
{
    if (event->type == ESP_WEBRTC_EVENT_CONNECTED) {
        s_connected = true;
        ESP_LOGI(TAG, "browser connected: the mic is live");
    } else if (event->type == ESP_WEBRTC_EVENT_CONNECT_FAILED || event->type == ESP_WEBRTC_EVENT_DISCONNECTED) {
        s_connected = false;
        s_active = false;
        s_channel = 0xFFFF;
        ESP_LOGI(TAG, "browser gone (%d)", event->type);
        esp_webrtc_enable_peer_connection(webrtc, false);
    }
    return 0;
}

/* once a second while connected: a small status line on the data channel */
static void status_thread(void *arg)
{
    while (webrtc) {
        media_lib_thread_sleep(1000);
        if (s_connected && s_channel != 0xFFFF) {
            char s[96];
            snprintf(s, sizeof s, "{\"pet\":\"D350\",\"uptime_s\":%lld}", esp_timer_get_time() / 1000000);
            pet_say(s);
        }
    }
    media_lib_thread_destroy(NULL);
}

int start_webrtc(void)
{
    if (webrtc) return 0;
    esp_peer_default_cfg_t peer_cfg = { .agent_recv_timeout = 500 };
    esp_webrtc_cfg_t cfg = {
        .peer_cfg = {
            .audio_info = { .codec = ESP_PEER_AUDIO_CODEC_OPUS, .sample_rate = 16000, .channel = 1 },
            .video_info = { .codec = ESP_PEER_VIDEO_CODEC_NONE },
            .audio_dir = ESP_PEER_MEDIA_DIR_SEND_ONLY,
            .video_dir = ESP_PEER_MEDIA_DIR_NONE,
            .on_custom_data = on_cmd,
            .on_channel_open = on_channel_open,
            .on_data = on_data,
            .on_channel_close = on_channel_close,
            .enable_data_channel = true,
            .no_auto_reconnect = true,
            .extra_cfg = &peer_cfg,
            .extra_size = sizeof(peer_cfg),
        },
        .peer_impl = esp_peer_get_default_impl(),
        .signaling_impl = esp_signaling_get_http_impl(),
    };
    int ret = esp_webrtc_open(&cfg, &webrtc);
    if (ret != 0) { ESP_LOGE(TAG, "esp_webrtc_open failed %d", ret); return ret; }
    esp_webrtc_media_provider_t provider = {};
    media_sys_get_provider(&provider);
    esp_webrtc_set_media_provider(webrtc, &provider);
    esp_webrtc_set_event_handler(webrtc, on_event, NULL);
    esp_webrtc_enable_peer_connection(webrtc, false);   /* wait for the page's LISTEN */
    ret = esp_webrtc_start(webrtc);
    if (ret != 0) { ESP_LOGE(TAG, "esp_webrtc_start failed %d", ret); return ret; }
    media_lib_thread_create_from_scheduler(NULL, "status", status_thread, NULL);
    return 0;
}

void query_webrtc(void)
{
    if (webrtc) esp_webrtc_query(webrtc);
}

int stop_webrtc(void)
{
    if (webrtc) {
        esp_webrtc_handle_t h = webrtc;
        webrtc = NULL;
        esp_webrtc_close(h);
    }
    return 0;
}
