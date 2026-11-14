/*
 * dv_ws.c — a small WebSocket client for the Discord gateways: TLS (certificate bundle) + ws from
 * ESP-IDF's tcp_transport, driven by the caller's task. esp_websocket_client is not used on purpose:
 * it always creates its own task in internal RAM, which the pet firmware does not have to spare.
 * Control frames are passed up to us (propagate_control_frames) so a CLOSE keeps its code and
 * reason: Discord says why it hung up there (4004 bad token, 4014 kicked, ...). Pings get a pong here.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dv_internal.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_transport.h"
#include "esp_transport_ssl.h"
#include "esp_transport_ws.h"

static const char *TAG = "dv_ws";

struct dv_ws {
    esp_transport_handle_t ssl, ws;
    int close_code;
    char close_reason[96];
};

dv_ws_t *dv_ws_open(const char *host, int port, const char *path, int timeout_ms)
{
    dv_ws_t *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->ssl = esp_transport_ssl_init();
    if (!c->ssl) { free(c); return NULL; }
    esp_transport_ssl_crt_bundle_attach(c->ssl, esp_crt_bundle_attach);
    c->ws = esp_transport_ws_init(c->ssl);
    if (!c->ws) { esp_transport_destroy(c->ssl); free(c); return NULL; }
    const esp_transport_ws_config_t wcfg = {
        .ws_path = path,
        .user_agent = "DiscordBot (https://github.com/Soul-Brews-Studio/esp32-oracle, 0.1)",
        .propagate_control_frames = true,
    };
    esp_transport_ws_set_config(c->ws, &wcfg);
    if (esp_transport_connect(c->ws, host, port, timeout_ms) < 0) {
        ESP_LOGW(TAG, "connect wss://%s:%d%s failed", host, port, path);
        dv_ws_close(c);
        return NULL;
    }
    return c;
}

bool dv_ws_send_text(dv_ws_t *c, const char *s)
{
    int n = (int)strlen(s);
    return esp_transport_ws_send_raw(c->ws, WS_TRANSPORT_OPCODES_TEXT | WS_TRANSPORT_OPCODES_FIN, s, n, 5000) == n;
}

int dv_ws_recv(dv_ws_t *c, char *buf, int cap, int timeout_ms, bool *binary)
{
    int pr = esp_transport_poll_read(c->ws, timeout_ms);
    if (pr < 0) return -1;
    if (pr == 0) return 0;
    int got = 0;
    for (;;) {                                         /* frames until FIN: one whole message */
        int r = esp_transport_read(c->ws, buf + got, cap - 1 - got, 5000);
        if (r < 0) return -1;
        ws_transport_opcodes_t op = esp_transport_ws_get_read_opcode(c->ws);
        if (op == WS_TRANSPORT_OPCODES_CLOSE) {
            const uint8_t *p = (const uint8_t *)buf + got;
            c->close_code = r >= 2 ? p[0] << 8 | p[1] : 1005;
            snprintf(c->close_reason, sizeof c->close_reason, "%.*s", r > 2 ? r - 2 : 0, (const char *)p + 2);
            ESP_LOGW(TAG, "closed by the server: %d %s", c->close_code, c->close_reason);
            return -1;
        }
        if (op == WS_TRANSPORT_OPCODES_PING || op == WS_TRANSPORT_OPCODES_PONG) {
            if (op == WS_TRANSPORT_OPCODES_PING)
                esp_transport_ws_send_raw(c->ws, WS_TRANSPORT_OPCODES_PONG | WS_TRANSPORT_OPCODES_FIN, buf + got, r, 5000);
            if (got == 0) return 0;                    /* nothing for the caller yet */
        } else {
            if (op == WS_TRANSPORT_OPCODES_BINARY) *binary = true;
            else if (op == WS_TRANSPORT_OPCODES_TEXT) *binary = false;
            /* the rest of this frame's payload (the length is per frame, not per message) */
            int want = esp_transport_ws_get_read_payload_len(c->ws), frame = r;
            got += r;
            while (frame < want && got < cap - 1) {
                r = esp_transport_read(c->ws, buf + got, cap - 1 - got, 5000);
                if (r <= 0) return -1;
                got += r;
                frame += r;
            }
            if (frame < want) {
                ESP_LOGE(TAG, "message larger than %d bytes", cap);
                return -1;
            }
            if (esp_transport_ws_get_fin_flag(c->ws)) break;
        }
        if (esp_transport_poll_read(c->ws, 5000) <= 0) return -1;   /* a continuation is coming */
    }
    buf[got] = 0;
    return got;
}

int dv_ws_close_code(const dv_ws_t *c, const char **reason)
{
    if (reason) *reason = c ? c->close_reason : "";
    return c ? c->close_code : 0;
}

void dv_ws_close(dv_ws_t *c)
{
    if (!c) return;
    if (c->ws) { esp_transport_close(c->ws); esp_transport_destroy(c->ws); }   /* ws_destroy leaves its parent */
    if (c->ssl) esp_transport_destroy(c->ssl);
    free(c);
}
