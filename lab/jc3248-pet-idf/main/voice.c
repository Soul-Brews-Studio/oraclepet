/*
 * voice.c — the pet's microphone to the voice server on the Mac (lab/oraclepet-lib/VOICE.md).
 *
 * audio.c cuts the mic into utterances (energy VAD, 300 ms pre-roll, 0.8 s hangover) and hands
 * them over as messages: {"type":"start"}, binary frames of whole 256-byte PSND blocks (4 per
 * frame, ~126 ms of audio), {"type":"end"} with the exact sample count. They wait in a PSRAM
 * message buffer; one network task sends them over a WebSocket and reads the server's
 * {"type":"heard","text":…}.
 *
 * Memory: with WiFi, BLE, WAMR and the GIF engine there is no internal RAM left for another
 * task stack ("Error create websocket task" with esp_websocket_client, which always puts its
 * stack there). So the network and mDNS tasks run on PSRAM stacks (they never touch flash) and
 * talk WebSocket through ESP-IDF's own tcp_transport.
 *
 * The mode (0 off, 1 stream speech) is kept in NVS "wifi"/"voice", so a pet that was listening
 * listens again after a reboot once WiFi is back. No URL from BLE: the server is found by mDNS
 * (_oraclepet._tcp, which it announces).
 */
#include "voice.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_transport.h"
#include "esp_transport_tcp.h"
#include "esp_transport_ws.h"
#include "freertos/FreeRTOS.h"
#include "freertos/message_buffer.h"
#include "freertos/task.h"
#include "mdns.h"
#include "nvs.h"
#include "wifi.h"

static const char *TAG = "voice";

static char  s_hello[200];
static int   s_mode;
static char  s_state[40] = "off";
static char  s_url[128];                       /* ws://host:port/path, from BLE or mDNS */
static volatile bool s_want;                   /* wifi up and voice on: the net task should run */
static volatile bool s_linked;                 /* the socket is open */
static MessageBufferHandle_t s_out;            /* audio task -> net task: [kind][payload] */
static TaskHandle_t s_net, s_mdns;
static uint8_t s_frame[4 * 256];
static size_t  s_frame_n;

enum { M_TEXT = 1, M_BIN = 2 };

/* ---- audio.c callbacks (audio task): queue, never block ---- */
static void put(uint8_t kind, const void *d, size_t n)
{
    static uint8_t m[1 + sizeof s_frame];
    if (!s_out || !s_linked || n > sizeof s_frame) return;
    m[0] = kind;
    memcpy(m + 1, d, n);
    if (xMessageBufferSend(s_out, m, 1 + n, 0) == 0)
        ESP_LOGW(TAG, "network too slow: %u bytes dropped", (unsigned)n);
}

static void flush(void) { if (s_frame_n) put(M_BIN, s_frame, s_frame_n); s_frame_n = 0; }

static void v_start(int id, int floor)
{
    char t[96];
    int n = snprintf(t, sizeof t, "{\"type\":\"start\",\"id\":%d,\"preroll_ms\":300,\"floor\":%d}", id, floor);
    s_frame_n = 0;
    put(M_TEXT, t, n);
}

static void v_block(const uint8_t *blk, size_t n)
{
    if (s_frame_n + n > sizeof s_frame) flush();
    memcpy(s_frame + s_frame_n, blk, n);
    s_frame_n += n;
    if (s_frame_n == sizeof s_frame) flush();
}

static void v_end(int id, uint32_t samples, const char *reason, int rms, int peak)
{
    flush();
    char t[160];
    int n = snprintf(t, sizeof t, "{\"type\":\"end\",\"id\":%d,\"samples\":%lu,\"reason\":\"%s\",\"rms\":%d,\"peak\":%d}",
                     id, (unsigned long)samples, reason, rms, peak);
    put(M_TEXT, t, n);
}

static const audio_voice_cb_t s_cb = { .start = v_start, .block = v_block, .end = v_end };

/* ---- the network task (PSRAM stack) ---- */
static bool parse_url(char *host, size_t hn, int *port, char *path, size_t pn)
{
    const char *p = s_url;
    if (strncmp(p, "ws://", 5) != 0) return false;
    p += 5;
    const char *slash = strchr(p, '/'), *colon = strchr(p, ':');
    if (colon && (!slash || colon < slash)) {
        snprintf(host, hn, "%.*s", (int)(colon - p), p);
        *port = atoi(colon + 1);
    } else {
        snprintf(host, hn, "%.*s", slash ? (int)(slash - p) : (int)strlen(p), p);
        *port = 80;
    }
    snprintf(path, pn, "%s", slash ? slash : "/");
    return host[0] && *port > 0;
}

static void net_task(void *arg)
{
    (void)arg;
    static uint8_t m[1 + sizeof s_frame];
    char host[64], path[64];
    int port;
    for (;;) {
        if (!s_want || !s_url[0] || !parse_url(host, sizeof host, &port, path, sizeof path)) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        esp_transport_handle_t tcp = esp_transport_tcp_init();
        esp_transport_handle_t ws = esp_transport_ws_init(tcp);
        esp_transport_ws_set_path(ws, path);
        snprintf(s_state, sizeof s_state, "connecting");
        if (esp_transport_connect(ws, host, port, 5000) < 0) {
            snprintf(s_state, sizeof s_state, "server not reachable");
            esp_transport_destroy(ws);
            esp_transport_destroy(tcp);
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }
        ESP_LOGI(TAG, "connected to %s (internal RAM free %u)", s_url, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        snprintf(s_state, sizeof s_state, "connected");
        esp_transport_ws_send_raw(ws, WS_TRANSPORT_OPCODES_TEXT | WS_TRANSPORT_OPCODES_FIN, s_hello, strlen(s_hello), 2000);
        xMessageBufferReset(s_out);
        s_linked = true;
        audio_voice_set(&s_cb);
        TickType_t last_ping = xTaskGetTickCount();
        while (s_want) {
            size_t n = xMessageBufferReceive(s_out, m, sizeof m, pdMS_TO_TICKS(50));
            if (n > 1) {
                int op = (m[0] == M_TEXT ? WS_TRANSPORT_OPCODES_TEXT : WS_TRANSPORT_OPCODES_BINARY) | WS_TRANSPORT_OPCODES_FIN;
                if (esp_transport_ws_send_raw(ws, op, (const char *)m + 1, n - 1, 2000) < 0) break;
            }
            if (xTaskGetTickCount() - last_ping > pdMS_TO_TICKS(10000)) {
                if (esp_transport_ws_send_raw(ws, WS_TRANSPORT_OPCODES_PING | WS_TRANSPORT_OPCODES_FIN, NULL, 0, 2000) < 0) break;
                last_ping = xTaskGetTickCount();
            }
            if (esp_transport_poll_read(ws, 0) > 0) {                 /* the server's "heard", pongs */
                static char rx[512];
                int r = esp_transport_read(ws, rx, sizeof rx - 1, 100);
                if (r < 0) break;
                if (r > 0 && esp_transport_ws_get_read_opcode(ws) == WS_TRANSPORT_OPCODES_TEXT) {
                    rx[r] = 0;
                    ESP_LOGI(TAG, "server: %s", rx);
                } else if (r == 0 && esp_transport_ws_get_read_opcode(ws) == WS_TRANSPORT_OPCODES_CLOSE) {
                    break;
                }
            }
        }
        s_linked = false;
        audio_voice_set(NULL);
        esp_transport_close(ws);
        esp_transport_destroy(ws);
        esp_transport_destroy(tcp);
        if (s_want) { snprintf(s_state, sizeof s_state, "reconnecting"); vTaskDelay(pdMS_TO_TICKS(2000)); }
    }
}

/* ---- finding the server by mDNS (PSRAM stack) ---- */
static void mdns_task(void *arg)
{
    (void)arg;
    static bool started;
    if (!started) started = mdns_init() == ESP_OK;
    while (s_want && !s_url[0]) {
        mdns_result_t *r = NULL;
        if (mdns_query_ptr("_oraclepet", "_tcp", 3000, 4, &r) == ESP_OK && r) {
            for (mdns_result_t *x = r; x && !s_url[0]; x = x->next) {
                for (mdns_ip_addr_t *a = x->addr; a; a = a->next) {
                    if (a->addr.type != ESP_IPADDR_TYPE_V4) continue;
                    const char *path = "/pet";
                    for (size_t k = 0; k < x->txt_count; k++)
                        if (strcmp(x->txt[k].key, "path") == 0 && x->txt[k].value) path = x->txt[k].value;
                    snprintf(s_url, sizeof s_url, "ws://" IPSTR ":%u%s", IP2STR(&a->addr.u_addr.ip4), x->port, path);
                    ESP_LOGI(TAG, "found the voice server: %s (%s)", s_url, x->instance_name ? x->instance_name : "?");
                    break;
                }
            }
            mdns_query_results_free(r);
        }
        if (!s_url[0]) { snprintf(s_state, sizeof s_state, "looking for the server"); vTaskDelay(pdMS_TO_TICKS(5000)); }
    }
    s_mdns = NULL;
    vTaskDeleteWithCaps(NULL);
}

static bool start_tasks(void)
{
    if (!s_out) s_out = xMessageBufferCreateWithCaps(32 * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_out) return false;
    if (!s_net && xTaskCreateWithCaps(net_task, "voice-net", 6144, NULL, 4, &s_net, MALLOC_CAP_SPIRAM) != pdPASS) {
        s_net = NULL;
        return false;
    }
    return true;
}

void voice_wifi_up(const char *url)
{
    if (!s_mode || !audio_has_mic()) return;
    if (!start_tasks()) { snprintf(s_state, sizeof s_state, "error: no memory"); return; }
    if (url && url[0]) snprintf(s_url, sizeof s_url, "%s", url);
    else s_url[0] = 0;                           /* find it again: the server may have moved */
    s_want = true;
    if (!s_url[0] && !s_mdns) {
        snprintf(s_state, sizeof s_state, "looking for the server");
        if (xTaskCreateWithCaps(mdns_task, "voice-mdns", 4096, NULL, 3, &s_mdns, MALLOC_CAP_SPIRAM) != pdPASS) s_mdns = NULL;
    }
}

void voice_wifi_down(void)
{
    s_want = false;                              /* the net task closes the socket and waits */
    if (s_mode) snprintf(s_state, sizeof s_state, "waiting for wifi");
}

static void save_mode(void)
{
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "voice", (uint8_t)s_mode);
    nvs_commit(h);
    nvs_close(h);
}

bool voice_set_mode(int mode)
{
    if (mode && !audio_has_mic()) return false;
    s_mode = mode ? 1 : 0;
    save_mode();
    if (!s_mode) { voice_wifi_down(); snprintf(s_state, sizeof s_state, "off"); return true; }
    if (wifi_is_up()) voice_wifi_up(wifi_url());
    else snprintf(s_state, sizeof s_state, "waiting for wifi");
    return true;
}

int voice_mode(void) { return s_mode; }
const char *voice_state(void) { return s_state; }

void voice_init(const char *pet_name, const char *project, const char *fw)
{
    snprintf(s_hello, sizeof s_hello,
             "{\"type\":\"hello\",\"pet\":\"%s\",\"project\":\"%s\",\"fw\":\"%s\",\"sr\":16000,\"codec\":\"ima-psnd\"}",
             pet_name, project, fw);
    nvs_handle_t h;
    uint8_t m = 0;
    if (nvs_open("wifi", NVS_READONLY, &h) == ESP_OK) { nvs_get_u8(h, "voice", &m); nvs_close(h); }
    s_mode = (m && audio_has_mic()) ? 1 : 0;
    if (s_mode) snprintf(s_state, sizeof s_state, "waiting for wifi");
}
