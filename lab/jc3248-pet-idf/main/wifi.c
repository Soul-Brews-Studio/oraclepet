/*
 * wifi.c — WiFi for the pet's voice (lab/oraclepet-lib/VOICE.md). BLE stays the control link;
 * WiFi carries the microphone to the voice server on the Mac.
 *
 * The SSID, password and server URL arrive over BLE (0x0D WIFI_CONFIG, ota.c) and live in NVS
 * namespace "wifi", so the pet reconnects by itself after a reboot. The password is never
 * logged and never sent back.
 */
#include "wifi.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "nvs.h"
#include "voice.h"

static const char *TAG = "wifi";

static char         s_url[128];
static char         s_state[48] = "off";
static bool         s_started, s_up;
static wifi_note_cb s_note;
static TimerHandle_t s_retry;
static volatile bool s_scanning;               /* a scan owns the radio: no reconnects meanwhile */

static void note(void)
{
    if (s_note) s_note(s_state);
}

static void retry_cb(TimerHandle_t t) { (void)t; esp_wifi_connect(); }

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = data;
        bool was_up = s_up;
        s_up = false;
        snprintf(s_state, sizeof s_state, "connecting (reason %d)", d ? d->reason : 0);
        if (was_up) voice_wifi_down();
        note();
        if (!s_scanning) xTimerStart(s_retry, 0);     /* try again in 5 s, for ever */
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        s_up = true;
        /* no modem sleep: waking for every beacon (~102 ms) pulsed the supply, and the mic
         * heard it as a 10 Hz rumble (floor 350 instead of ~12), too loud for the VAD */
        esp_wifi_set_ps(WIFI_PS_NONE);
        snprintf(s_state, sizeof s_state, "up " IPSTR, IP2STR(&e->ip_info.ip));
        ESP_LOGI(TAG, "%s", s_state);
        note();
        voice_wifi_up(s_url);
    }
}

static bool load(char *ssid, size_t ns, char *pass, size_t np)
{
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READONLY, &h) != ESP_OK) return false;
    size_t a = ns, b = np, c = sizeof s_url;
    bool ok = nvs_get_str(h, "ssid", ssid, &a) == ESP_OK && nvs_get_str(h, "pass", pass, &b) == ESP_OK;
    if (nvs_get_str(h, "url", s_url, &c) != ESP_OK) s_url[0] = 0;
    nvs_close(h);
    return ok && ssid[0];
}

static void connect_to(const char *ssid, const char *pass)
{
    if (!s_started) {
        esp_netif_init();
        esp_err_t e = esp_event_loop_create_default();
        if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) { snprintf(s_state, sizeof s_state, "error: event loop"); return; }
        esp_netif_create_default_wifi_sta();
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        if (esp_wifi_init(&cfg) != ESP_OK) { snprintf(s_state, sizeof s_state, "error: wifi init"); note(); return; }
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
        esp_wifi_set_storage(WIFI_STORAGE_RAM);       /* the credentials live in our own NVS keys */
        esp_wifi_set_mode(WIFI_MODE_STA);
        s_retry = xTimerCreate("wifi-retry", pdMS_TO_TICKS(5000), pdFALSE, NULL, retry_cb);
    }
    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, ssid, sizeof wc.sta.ssid);
    strlcpy((char *)wc.sta.password, pass, sizeof wc.sta.password);
    wc.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    snprintf(s_state, sizeof s_state, "connecting");
    note();
    if (!s_started) { esp_wifi_start(); s_started = true; }    /* STA_START connects */
    else { esp_wifi_disconnect(); esp_wifi_connect(); }
    ESP_LOGI(TAG, "connecting to '%s'", ssid);
}

void wifi_init(wifi_note_cb cb)
{
    s_note = cb;
    char ssid[33], pass[65];
    if (load(ssid, sizeof ssid, pass, sizeof pass)) connect_to(ssid, pass);
    else ESP_LOGI(TAG, "no WiFi configured (OraclePet sends it over BLE)");
}

bool wifi_set(const char *ssid, const char *pass, const char *url)
{
    if (!ssid || !ssid[0] || strlen(ssid) > 32 || strlen(pass) > 64 || strlen(url) >= sizeof s_url) return false;
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_str(h, "ssid", ssid) == ESP_OK && nvs_set_str(h, "pass", pass) == ESP_OK &&
              nvs_set_str(h, "url", url) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    if (!ok) return false;
    strlcpy(s_url, url, sizeof s_url);
    voice_wifi_down();                                /* a new server URL: reconnect the socket */
    connect_to(ssid, pass);
    return true;
}

const char *wifi_state(void) { return s_state; }
bool        wifi_is_up(void) { return s_up; }
const char *wifi_url(void)   { return s_url; }

/* ---- scan: what networks can the pet see (2.4 GHz only, the S3 has no 5 GHz) ---- */
static wifi_found_cb s_found;

static void ensure_started(void)
{
    if (s_started) return;
    esp_netif_init();
    esp_err_t e = esp_event_loop_create_default();
    (void)e;
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&cfg) != ESP_OK) return;
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);
    s_retry = xTimerCreate("wifi-retry", pdMS_TO_TICKS(5000), pdFALSE, NULL, retry_cb);
    esp_wifi_start();
    s_started = true;
}

static void scan_task(void *arg)
{
    (void)arg;
    ensure_started();
    xTimerStop(s_retry, 0);                       /* a pending reconnect would cancel the scan */
    esp_wifi_disconnect();
    wifi_scan_config_t sc = { .show_hidden = false };
    if (esp_wifi_scan_start(&sc, true) == ESP_OK) {
        uint16_t n = 20;
        static wifi_ap_record_t ap[20];
        if (esp_wifi_scan_get_ap_records(&n, ap) == ESP_OK) {
            for (int i = 0; i < n; i++) {
                if (!ap[i].ssid[0]) continue;
                bool dup = false;                 /* one line per name (meshes repeat it) */
                for (int k = 0; k < i; k++) if (strcmp((char *)ap[k].ssid, (char *)ap[i].ssid) == 0) dup = true;
                if (!dup && s_found) s_found((char *)ap[i].ssid, ap[i].rssi, ap[i].primary, ap[i].authmode == WIFI_AUTH_OPEN);
            }
            ESP_LOGI(TAG, "scan: %u networks", n);
        }
    }
    s_scanning = false;
    esp_wifi_connect();                           /* back to the saved network, if any */
    vTaskDelete(NULL);
}

bool wifi_scan(wifi_found_cb found)
{
    if (s_scanning) return false;
    s_scanning = true;
    s_found = found;
    return xTaskCreate(scan_task, "wifi-scan", 4096, NULL, 4, NULL) == pdPASS;
}
