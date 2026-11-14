/* dv_internal.h — shared between the gateway, the voice link and the tiny WebSocket client. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "discord_voice.h"

/* ---- dv_ws.c: a WebSocket client over esp_transport (TLS + ws), no task of its own ---- */
typedef struct dv_ws dv_ws_t;
dv_ws_t *dv_ws_open(const char *host, int port, const char *path, int timeout_ms);
bool     dv_ws_send_text(dv_ws_t *ws, const char *s);
/* One whole message into buf (NUL-terminated for text). Returns its length, 0 on timeout,
 * -1 when the socket closed or failed. *binary tells text from binary (DAVE frames). */
int      dv_ws_recv(dv_ws_t *ws, char *buf, int cap, int timeout_ms, bool *binary);
/* After dv_ws_recv() returned -1: the server's close code (0 if it never sent one) and reason. */
int      dv_ws_close_code(const dv_ws_t *ws, const char **reason);
void     dv_ws_close(dv_ws_t *ws);

/* ---- dv_gateway.c -> dv_voice.c: what the gateway learned for the voice link ---- */
typedef struct {
    char user_id[24];
    char guild_id[24];
    char session_id[64];
    char token[64];
    char endpoint[128];               /* host[:port] of the voice server */
} dv_voice_creds_t;

void dv_voice_begin(const dv_voice_creds_t *creds);   /* (re)start the voice link with these */
void dv_voice_stop(void);
void dv_emit(dv_event_t evt, const char *text);
void dv_set_state(const char *fmt, ...);
