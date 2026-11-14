/*
 * main.c — discord-voice-esp32 lab, milestone M1: the board joins a Discord voice channel as its
 * own bot and plays a test melody, with no PC in between. WiFi, the bot token, the guild and the
 * voice channel come from menuconfig ("OraclePet Discord voice lab") and live only in sdkconfig,
 * which is gitignored.
 */
#include <math.h>
#include <string.h>

#include "discord_voice.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "dvl";

static EventGroupHandle_t s_net;
#define GOT_IP BIT0

static void on_net(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_net, GOT_IP);
        ESP_LOGW(TAG, "wifi: disconnected, trying again");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "wifi: got " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_net, GOT_IP);
    }
}

static void wifi_start(void)
{
    s_net = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_net, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_net, NULL));
    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, CONFIG_DVL_WIFI_SSID, sizeof wc.sta.ssid);
    strlcpy((char *)wc.sta.password, CONFIG_DVL_WIFI_PASSWORD, sizeof wc.sta.password);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);                 /* no BLE in this lab: keep the 20 ms cadence smooth */
}

static void on_dv(dv_event_t evt, const char *text)
{
    static const char *names[] = { "gateway ready", "voice ready", "DAVE required", "error", "disconnected" };
    ESP_LOGI(TAG, "discord: %s %s", evt <= DV_EVT_DISCONNECTED ? names[evt] : "?", text);
}

/* M1's sound: a C-major arpeggio, 4 s on and 1 s off, so speaking on/off is exercised too.
 * 16 kHz mono, one 20 ms frame per period; the stack is in PSRAM (the Opus encoder runs here). */
static void melody_task(void *arg)
{
    (void)arg;
    static const float notes[] = { 261.63f, 329.63f, 392.00f, 523.25f, 392.00f, 329.63f };
    int16_t pcm[DV_FRAME_SAMPLES];
    float phase = 0;
    TickType_t last = xTaskGetTickCount();
    for (uint32_t frame = 0;; frame++) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(20));
        uint32_t t = frame % 250;                  /* 200 frames of sound, 50 quiet */
        if (!dv_voice_ready()) continue;
        if (t >= 200) {
            if (t == 200) dv_silence();
            continue;
        }
        int n = t % 33;                            /* 33 frames (0.66 s) per note, soft edges */
        float f = notes[(t / 33) % 6];
        float amp = 6000.0f * (n < 2 ? (n + 1) / 3.0f : n > 30 ? (33 - n) / 3.0f : 1.0f);
        for (int i = 0; i < DV_FRAME_SAMPLES; i++) {
            pcm[i] = (int16_t)(amp * sinf(phase));
            phase += 2.0f * (float)M_PI * f / 16000.0f;
            if (phase > 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
        }
        dv_send_pcm(pcm);
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    if (!CONFIG_DVL_WIFI_SSID[0] || !CONFIG_DVL_BOT_TOKEN[0] || !CONFIG_DVL_GUILD_ID[0] || !CONFIG_DVL_CHANNEL_ID[0]) {
        ESP_LOGE(TAG, "set WiFi, bot token, guild and channel: idf.py menuconfig, \"OraclePet Discord voice lab\"");
        return;
    }
    wifi_start();
    xEventGroupWaitBits(s_net, GOT_IP, pdFALSE, pdTRUE, portMAX_DELAY);
    const dv_config_t cfg = {
        .token = CONFIG_DVL_BOT_TOKEN,
        .guild_id = CONFIG_DVL_GUILD_ID,
        .channel_id = CONFIG_DVL_CHANNEL_ID,
        .on_event = on_dv,
    };
    if (!dv_start(&cfg)) {
        ESP_LOGE(TAG, "dv_start failed");
        return;
    }
    xTaskCreateWithCaps(melody_task, "melody", 40 * 1024, NULL, 5, NULL, MALLOC_CAP_SPIRAM);
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "state %s, free internal %u, psram %u", dv_state(),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }
}
