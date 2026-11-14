/*
 * dv_gateway.c — the Discord gateway (v10, JSON): log the bot in, keep the heartbeat, ask to join
 * the voice channel (op 4) and collect what the voice link needs from VOICE_STATE_UPDATE (our voice
 * session id) and VOICE_SERVER_UPDATE (token, endpoint). Runs on one task with a PSRAM stack;
 * on any error it closes, waits and logs in again (no resume: this is a lab).
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "dv_internal.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "dv_gw";

#define GW_HOST "gateway.discord.gg"
#define GW_PATH "/?v=10&encoding=json"
#define GW_BUF  (64 * 1024)
#define INTENTS 128                                    /* GUILD_VOICE_STATES: our own voice state */

static dv_config_t s_cfg;
static dv_voice_creds_t s_creds;
static char s_state[48] = "idle";

void dv_set_state(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_state, sizeof s_state, fmt, ap);
    va_end(ap);
}

const char *dv_state(void) { return s_state; }

void dv_emit(dv_event_t evt, const char *text)
{
    if (evt == DV_EVT_ERROR) ESP_LOGW(TAG, "%s", text ? text : "");
    if (s_cfg.on_event) s_cfg.on_event(evt, text ? text : "");
}

static const char *jstr(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static void send_json(dv_ws_t *ws, cJSON *msg)
{
    char *s = cJSON_PrintUnformatted(msg);
    if (s) { dv_ws_send_text(ws, s); free(s); }
    cJSON_Delete(msg);
}

static void identify(dv_ws_t *ws)
{
    cJSON *m = cJSON_CreateObject(), *d = cJSON_AddObjectToObject(m, "d"), *p;
    cJSON_AddNumberToObject(m, "op", 2);
    cJSON_AddStringToObject(d, "token", s_cfg.token);
    cJSON_AddNumberToObject(d, "intents", INTENTS);
    p = cJSON_AddObjectToObject(d, "properties");
    cJSON_AddStringToObject(p, "os", "esp-idf");
    cJSON_AddStringToObject(p, "browser", "oraclepet");
    cJSON_AddStringToObject(p, "device", "oraclepet");
    send_json(ws, m);
}

static void join_voice(dv_ws_t *ws)
{
    cJSON *m = cJSON_CreateObject(), *d = cJSON_AddObjectToObject(m, "d");
    cJSON_AddNumberToObject(m, "op", 4);
    cJSON_AddStringToObject(d, "guild_id", s_cfg.guild_id);
    cJSON_AddStringToObject(d, "channel_id", s_cfg.channel_id);
    cJSON_AddBoolToObject(d, "self_mute", false);
    cJSON_AddBoolToObject(d, "self_deaf", true);       /* M1/M2 only send; deaf saves the downlink */
    send_json(ws, m);
}

static void heartbeat(dv_ws_t *ws, int seq)
{
    char s[48];
    if (seq < 0) snprintf(s, sizeof s, "{\"op\":1,\"d\":null}");
    else snprintf(s, sizeof s, "{\"op\":1,\"d\":%d}", seq);
    dv_ws_send_text(ws, s);
}

/* close codes after which logging in again cannot help: bad token, bad shard, version or intents */
static bool fatal_close(int code)
{
    return code == 4004 || (code >= 4010 && code <= 4014);
}

static void gw_task(void *arg)
{
    (void)arg;
    char *buf = heap_caps_malloc(GW_BUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { dv_emit(DV_EVT_ERROR, "gateway: no memory"); vTaskDeleteWithCaps(NULL); }
    int backoff_ms = 3000;                             /* doubles per failed login, up to 2 min */
    for (;;) {
        dv_set_state("gateway");
        dv_ws_t *ws = dv_ws_open(GW_HOST, 443, GW_PATH, 10000);
        if (!ws) {
            dv_emit(DV_EVT_ERROR, "gateway: cannot connect");
            vTaskDelay(pdMS_TO_TICKS(backoff_ms));
            if (backoff_ms < 120000) backoff_ms *= 2;
            continue;
        }
        int seq = -1, hb_ms = 41250, close_code = 0;
        bool have_state = false, have_server = false;
        int64_t next_hb = 0;
        for (;;) {
            int64_t now = esp_timer_get_time() / 1000;
            int wait = next_hb ? (int)(next_hb - now) : 10000;
            if (wait < 0) wait = 0;
            bool bin = false;
            int n = dv_ws_recv(ws, buf, GW_BUF, wait, &bin);
            if (n < 0) {
                const char *why = "";
                close_code = dv_ws_close_code(ws, &why);
                char t[128];
                snprintf(t, sizeof t, "gateway closed: %d %s", close_code, why);
                dv_emit(DV_EVT_ERROR, t);
                break;
            }
            now = esp_timer_get_time() / 1000;
            if (next_hb && now >= next_hb) { heartbeat(ws, seq); next_hb = now + hb_ms; }
            if (n == 0 || bin) continue;
            cJSON *msg = cJSON_ParseWithLength(buf, n);
            if (!msg) continue;
            int op = cJSON_GetObjectItem(msg, "op") ? cJSON_GetObjectItem(msg, "op")->valueint : -1;
            const cJSON *s = cJSON_GetObjectItem(msg, "s"), *d = cJSON_GetObjectItem(msg, "d");
            if (cJSON_IsNumber(s)) seq = s->valueint;
            const char *t = jstr(msg, "t");
            bool stop = false;
            switch (op) {
            case 10: {                                 /* Hello */
                const cJSON *hi = cJSON_GetObjectItem(d, "heartbeat_interval");
                if (cJSON_IsNumber(hi)) hb_ms = hi->valueint;
                next_hb = now + (int)(hb_ms * (esp_random() % 1000) / 1000);   /* first beat with jitter */
                identify(ws);
                dv_set_state("identifying");
                break;
            }
            case 0:                                    /* Dispatch */
                if (t && strcmp(t, "READY") == 0) {
                    const cJSON *u = cJSON_GetObjectItem(d, "user");
                    const char *id = jstr(u, "id");
                    snprintf(s_creds.user_id, sizeof s_creds.user_id, "%s", id ? id : "");
                    snprintf(s_creds.guild_id, sizeof s_creds.guild_id, "%s", s_cfg.guild_id);
                    ESP_LOGI(TAG, "READY as %s (%s)", jstr(u, "username") ? jstr(u, "username") : "?", s_creds.user_id);
                    backoff_ms = 3000;
                    dv_emit(DV_EVT_GATEWAY_READY, s_creds.user_id);
                    join_voice(ws);
                    dv_set_state("joining");
                } else if (t && strcmp(t, "VOICE_STATE_UPDATE") == 0) {
                    const char *uid = jstr(d, "user_id"), *sid = jstr(d, "session_id"), *gid = jstr(d, "guild_id");
                    if (uid && sid && gid && strcmp(uid, s_creds.user_id) == 0 && strcmp(gid, s_cfg.guild_id) == 0) {
                        snprintf(s_creds.session_id, sizeof s_creds.session_id, "%s", sid);
                        have_state = true;
                    }
                } else if (t && strcmp(t, "VOICE_SERVER_UPDATE") == 0) {
                    const char *tok = jstr(d, "token"), *ep = jstr(d, "endpoint"), *gid = jstr(d, "guild_id");
                    if (tok && ep && gid && strcmp(gid, s_cfg.guild_id) == 0) {
                        snprintf(s_creds.token, sizeof s_creds.token, "%s", tok);
                        snprintf(s_creds.endpoint, sizeof s_creds.endpoint, "%s", ep);
                        have_server = true;
                    }
                }
                if ((t && (strcmp(t, "VOICE_STATE_UPDATE") == 0 || strcmp(t, "VOICE_SERVER_UPDATE") == 0))
                    && have_state && have_server) {
                    ESP_LOGI(TAG, "voice server %s, session %.8s…", s_creds.endpoint, s_creds.session_id);
                    dv_set_state("voice");
                    dv_voice_begin(&s_creds);
                    have_server = false;               /* a new server update starts a new link */
                }
                break;
            case 1: heartbeat(ws, seq); break;         /* the server asked for a beat now */
            case 7: stop = true; break;                /* Reconnect */
            case 9:                                    /* Invalid Session */
                dv_emit(DV_EVT_ERROR, "gateway: invalid session (token? intents?)");
                vTaskDelay(pdMS_TO_TICKS(3000));
                stop = true;
                break;
            default: break;                            /* 11 = heartbeat ACK */
            }
            cJSON_Delete(msg);
            if (stop) break;
        }
        dv_ws_close(ws);
        dv_voice_stop();
        dv_emit(DV_EVT_DISCONNECTED, "gateway");
        if (fatal_close(close_code)) {                 /* the settings are wrong: say so, try rarely */
            dv_set_state("error: gateway %d", close_code);
            ESP_LOGE(TAG, "gateway refused us (%d): check the token and intents; next try in 10 min", close_code);
            vTaskDelay(pdMS_TO_TICKS(10 * 60 * 1000));
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(backoff_ms));
        if (backoff_ms < 120000) backoff_ms *= 2;
    }
}

bool dv_start(const dv_config_t *cfg)
{
    if (!cfg || !cfg->token || !cfg->token[0] || !cfg->guild_id || !cfg->channel_id) return false;
    s_cfg = *cfg;
    s_cfg.token = strdup(cfg->token);
    s_cfg.guild_id = strdup(cfg->guild_id);
    s_cfg.channel_id = strdup(cfg->channel_id);
    return xTaskCreateWithCaps(gw_task, "dv_gw", 8192, NULL, 5, NULL, MALLOC_CAP_SPIRAM) == pdPASS;
}
