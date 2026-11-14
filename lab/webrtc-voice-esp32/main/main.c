/*
 * main.c — the pet's voice over WebRTC on the LAN. WiFi comes from the pet's own NVS (what it got
 * over Improv); once online the pet serves https://<ip>/webrtc/test and waits for a browser.
 * The thread tuning follows esp-webrtc-solution doorbell_local (Public Domain / CC0).
 */
#include <string.h>

#include "common.h"
#include "esp_capture.h"
#include "esp_log.h"
#include "media_lib_adapter.h"
#include "media_lib_netif.h"
#include "media_lib_os.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "PET_RTC_MAIN";

static void thread_scheduler(const char *name, media_lib_thread_cfg_t *cfg)
{
    if (strcmp(name, "pc_task") == 0) {
        cfg->stack_size = 25 * 1024;
        cfg->priority = 18;
        cfg->core_id = 1;
    } else if (strcmp(name, "aenc_0") == 0) {          /* the Opus encoder needs a big stack */
        cfg->stack_size = 40 * 1024;
        cfg->priority = 10;
        cfg->core_id = 1;
    } else if (strcmp(name, "AUD_SRC") == 0) {
        cfg->priority = 15;
    } else if (strcmp(name, "start") == 0) {
        cfg->stack_size = 6 * 1024;
    }
}

static void capture_scheduler(const char *name, esp_capture_thread_schedule_cfg_t *s)
{
    media_lib_thread_cfg_t cfg = { .stack_size = s->stack_size, .priority = s->priority, .core_id = s->core_id };
    s->stack_in_ext = true;
    thread_scheduler(name, &cfg);
    s->stack_size = cfg.stack_size;
    s->priority = cfg.priority;
    s->core_id = cfg.core_id;
}

static void start_task(void *arg)
{
    start_webrtc();
    media_lib_ipv4_info_t ip;
    media_lib_netif_get_ipv4_info(MEDIA_LIB_NET_TYPE_STA, &ip);
    ESP_LOGI(TAG, "open https://%s/webrtc/test to hear the pet", media_lib_ipv4_ntoa(&ip.ip));
    media_lib_thread_destroy(NULL);
}

static int on_network(bool connected)
{
    if (connected) media_lib_thread_create_from_scheduler(NULL, "start", start_task, NULL);
    else stop_webrtc();
    return 0;
}

/* the WiFi the pet firmware stored after Improv: NVS "wifi" / "ssid", "pass" */
static bool pet_wifi(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READONLY, &h) != ESP_OK) return false;
    bool ok = nvs_get_str(h, "ssid", ssid, &ssid_len) == ESP_OK && nvs_get_str(h, "pass", pass, &pass_len) == ESP_OK;
    nvs_close(h);
    return ok;
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGE(TAG, "NVS needs erasing: refusing, the pet's WiFi lives there");
        return;
    }
    static char ssid[33], pass[65];
    if (!pet_wifi(ssid, sizeof ssid, pass, sizeof pass)) {
        ESP_LOGE(TAG, "no WiFi in NVS: give the pet its WiFi over Improv first");
        return;
    }
    media_lib_add_default_adapter();
    esp_capture_set_thread_scheduler(capture_scheduler);
    media_lib_thread_set_schedule_cb(thread_scheduler);
    if (media_sys_buildup() != 0) {
        ESP_LOGE(TAG, "mic or capture failed");
        return;
    }
    network_init(ssid, pass, on_network);
    while (1) {
        media_lib_thread_sleep(5000);
        query_webrtc();
    }
}
