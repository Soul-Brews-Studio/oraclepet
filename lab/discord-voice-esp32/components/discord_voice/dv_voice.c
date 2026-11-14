/*
 * dv_voice.c — the Discord voice link (voice gateway v8 + UDP media), as @discordjs/voice 0.19.2 does it:
 *
 *   wss://<endpoint>/?v=8   op 0 Identify (max_dave_protocol_version 0) → op 8 Hello → op 2 Ready
 *   UDP                     IP discovery (74 bytes) → op 1 Select Protocol (aead_aes256_gcm_rtpsize)
 *                           → op 4 Session Description (secret_key) → op 5 Speaking
 *   media, every 20 ms      RTP 80 78 | seq | ts (+960) | ssrc, then AES-256-GCM(opus) with nonce
 *                           = 4-byte BE counter + 8 zero bytes, AAD = the header; header‖ct‖tag‖counter
 *
 * One task (PSRAM stack) owns the WebSocket; dv_send_pcm() runs on the caller's task, encodes and
 * sends UDP directly, and asks the voice task to send the Speaking flag (the TLS socket is not shared).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cJSON.h"
#include "dv_internal.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_opus_enc.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "psa/crypto.h"

static const char *TAG = "dv_voice";

#define VBUF       (16 * 1024)
#define MODE       "aead_aes256_gcm_rtpsize"
#define MAX_PACKET 512                             /* Opus payload; 20 ms at 32 kbit/s is ~80 bytes */

/* the link the voice task builds and dv_send_pcm() uses */
static struct {
    SemaphoreHandle_t mx;
    volatile bool ready;
    int udp;
    struct sockaddr_in dest;
    uint32_t ssrc, ts, nonce;
    uint16_t seq;
    psa_key_id_t key;
    void *opus;
    volatile int speaking_want;                    /* -1 nothing, 0/1: the voice task sends op 5 */
    bool speaking;
} L = { .udp = -1, .speaking_want = -1 };

static dv_voice_creds_t s_creds;
static TaskHandle_t s_task;
static volatile bool s_begin, s_stop;

bool dv_voice_ready(void) { return L.ready; }

/* ---- the media path ---- */

static void teardown(void)
{
    xSemaphoreTake(L.mx, portMAX_DELAY);
    L.ready = false;
    if (L.udp >= 0) { close(L.udp); L.udp = -1; }
    if (L.key) { psa_destroy_key(L.key); L.key = 0; }
    L.speaking = false;
    L.speaking_want = -1;
    xSemaphoreGive(L.mx);
}

/* one RTP packet: header, then AES-256-GCM over the Opus payload with the header as AAD */
static void send_rtp(const uint8_t *payload, size_t n)
{
    uint8_t pkt[12 + MAX_PACKET + 16 + 4];
    if (n > MAX_PACKET) return;
    pkt[0] = 0x80;
    pkt[1] = 0x78;                                 /* payload type 120: Opus */
    pkt[2] = L.seq >> 8; pkt[3] = L.seq;
    pkt[4] = L.ts >> 24; pkt[5] = L.ts >> 16; pkt[6] = L.ts >> 8; pkt[7] = L.ts;
    pkt[8] = L.ssrc >> 24; pkt[9] = L.ssrc >> 16; pkt[10] = L.ssrc >> 8; pkt[11] = L.ssrc;
    L.nonce++;                                     /* incremented before use, as @discordjs/voice */
    uint8_t nonce[12] = { L.nonce >> 24, L.nonce >> 16, L.nonce >> 8, L.nonce };
    size_t out = 0;
    psa_status_t st = psa_aead_encrypt(L.key, PSA_ALG_GCM, nonce, sizeof nonce, pkt, 12, payload, n,
                                       pkt + 12, n + 16, &out);
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_aead_encrypt: %d", (int)st);
        return;
    }
    memcpy(pkt + 12 + out, nonce, 4);              /* the counter rides at the end */
    sendto(L.udp, pkt, 12 + out + 4, 0, (struct sockaddr *)&L.dest, sizeof L.dest);
    L.seq++;
    L.ts += 960;                                   /* 20 ms at Opus' 48 kHz clock */
}

void dv_send_pcm(const int16_t *pcm)
{
    if (!L.mx || !L.ready) return;
    xSemaphoreTake(L.mx, portMAX_DELAY);
    if (L.ready && L.opus) {
        uint8_t out[MAX_PACKET];
        esp_audio_enc_in_frame_t in = { .buffer = (uint8_t *)pcm, .len = DV_FRAME_SAMPLES * 2 };
        esp_audio_enc_out_frame_t o = { .buffer = out, .len = sizeof out };
        if (esp_opus_enc_process(L.opus, &in, &o) == ESP_AUDIO_ERR_OK && o.encoded_bytes > 0) {
            if (!L.speaking) { L.speaking = true; L.speaking_want = 1; }
            send_rtp(out, o.encoded_bytes);
        }
    }
    xSemaphoreGive(L.mx);
}

void dv_silence(void)
{
    if (!L.mx || !L.ready) return;
    static const uint8_t silence[3] = { 0xF8, 0xFF, 0xFE };
    xSemaphoreTake(L.mx, portMAX_DELAY);
    if (L.ready && L.speaking) {
        for (int i = 0; i < 5; i++) send_rtp(silence, sizeof silence);
        L.speaking = false;
        L.speaking_want = 0;
    }
    xSemaphoreGive(L.mx);
}

/* Opus at 16 kHz mono: the stream is valid Opus, Discord's decoders play it at 48 kHz */
static bool open_opus(void)
{
    if (L.opus) return true;
    esp_opus_enc_config_t cfg = {
        .sample_rate = ESP_AUDIO_SAMPLE_RATE_16K,
        .channel = ESP_AUDIO_MONO,
        .bits_per_sample = ESP_AUDIO_BIT16,
        .bitrate = 32000,
        .frame_duration = ESP_OPUS_ENC_FRAME_DURATION_20_MS,
        .application_mode = ESP_OPUS_ENC_APPLICATION_VOIP,
        .complexity = 0,
        .enable_fec = false,
        .enable_dtx = false,
        .enable_vbr = true,
    };
    if (esp_opus_enc_open(&cfg, sizeof cfg, &L.opus) != ESP_AUDIO_ERR_OK || !L.opus) return false;
    int in = 0, out = 0;
    esp_opus_enc_get_frame_size(L.opus, &in, &out);
    ESP_LOGI(TAG, "opus: %d bytes in, up to %d out per 20 ms", in, out);
    if (in != DV_FRAME_SAMPLES * 2) ESP_LOGW(TAG, "opus wants %d bytes per frame, we send %d", in, DV_FRAME_SAMPLES * 2);
    return true;
}

/* ---- UDP: socket + IP discovery ---- */

static bool udp_discover(const char *ip, int port, char *my_ip, size_t my_ip_len, int *my_port)
{
    L.udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (L.udp < 0) return false;
    memset(&L.dest, 0, sizeof L.dest);
    L.dest.sin_family = AF_INET;
    L.dest.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &L.dest.sin_addr) != 1) return false;
    struct timeval tv = { .tv_sec = 2 };
    setsockopt(L.udp, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    uint8_t q[74] = { 0, 1, 0, 70, L.ssrc >> 24, L.ssrc >> 16, L.ssrc >> 8, L.ssrc };   /* type 1, length 70 */
    for (int tries = 0; tries < 3; tries++) {
        sendto(L.udp, q, sizeof q, 0, (struct sockaddr *)&L.dest, sizeof L.dest);
        uint8_t r[74];
        int n = recv(L.udp, r, sizeof r, 0);
        if (n == 74 && r[0] == 0 && r[1] == 2) {   /* type 2: response */
            r[71] = 0;
            snprintf(my_ip, my_ip_len, "%s", (const char *)r + 8);
            *my_port = r[72] << 8 | r[73];
            return true;
        }
    }
    return false;
}

/* ---- the voice gateway ---- */

static int jint(const cJSON *o, const char *k, int dflt)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? (int)v->valuedouble : dflt;
}

static const char *close_name(int code)
{
    switch (code) {
    case 4004: return "authentication failed";
    case 4006: return "session no longer valid";
    case 4009: return "session timeout";
    case 4011: return "server not found";
    case 4014: return "disconnected (kicked, moved or channel deleted)";
    case 4015: return "voice server crashed";
    case 4016: return "unknown encryption mode";
    case 4017: return "DAVE (E2EE) required";
    case 4021: return "rate limited";
    case 4022: return "call terminated";
    default:   return "";
    }
}

static void v_identify(dv_ws_t *ws)
{
    cJSON *m = cJSON_CreateObject(), *d = cJSON_AddObjectToObject(m, "d");
    cJSON_AddNumberToObject(m, "op", 0);
    cJSON_AddStringToObject(d, "server_id", s_creds.guild_id);
    cJSON_AddStringToObject(d, "user_id", s_creds.user_id);
    cJSON_AddStringToObject(d, "session_id", s_creds.session_id);
    cJSON_AddStringToObject(d, "token", s_creds.token);
    cJSON_AddNumberToObject(d, "max_dave_protocol_version", 0);   /* no DAVE in this build */
    char *s = cJSON_PrintUnformatted(m);
    if (s) { dv_ws_send_text(ws, s); free(s); }
    cJSON_Delete(m);
}

static void v_speaking(dv_ws_t *ws, int on)
{
    char s[96];
    snprintf(s, sizeof s, "{\"op\":5,\"d\":{\"speaking\":%d,\"delay\":0,\"ssrc\":%lu}}", on, (unsigned long)L.ssrc);
    dv_ws_send_text(ws, s);
}

/* op 2 Ready: open UDP, learn our public address, pick the cipher. false: give up on this link. */
static bool on_ready(dv_ws_t *ws, const cJSON *d)
{
    const cJSON *ipj = cJSON_GetObjectItemCaseSensitive(d, "ip");
    int uport = jint(d, "port", 0);
    L.ssrc = (uint32_t)jint(d, "ssrc", 0);
    bool gcm = false;
    const cJSON *mode;
    cJSON_ArrayForEach(mode, cJSON_GetObjectItemCaseSensitive(d, "modes"))
        if (cJSON_IsString(mode) && strcmp(mode->valuestring, MODE) == 0) gcm = true;
    if (!cJSON_IsString(ipj) || !uport) { dv_emit(DV_EVT_ERROR, "voice: Ready without ip/port"); return false; }
    if (!gcm) { dv_emit(DV_EVT_ERROR, "voice: server does not offer " MODE); return false; }
    char my_ip[64];
    int my_port = 0;
    if (!udp_discover(ipj->valuestring, uport, my_ip, sizeof my_ip, &my_port)) {
        dv_emit(DV_EVT_ERROR, "voice: UDP IP discovery got no answer");
        return false;
    }
    ESP_LOGI(TAG, "ssrc %lu, media %s:%d, we are %s:%d", (unsigned long)L.ssrc, ipj->valuestring, uport, my_ip, my_port);
    char sel[224];
    snprintf(sel, sizeof sel,
             "{\"op\":1,\"d\":{\"protocol\":\"udp\",\"data\":{\"address\":\"%s\",\"port\":%d,\"mode\":\"" MODE "\"}}}",
             my_ip, my_port);
    dv_ws_send_text(ws, sel);
    dv_set_state("selecting");
    return true;
}

/* op 4 Session Description: the media key. false: give up on this link. */
static bool on_session(const cJSON *d)
{
    int dave = jint(d, "dave_protocol_version", 0);
    if (dave > 0) {
        ESP_LOGE(TAG, "Discord asks for DAVE protocol %d: E2EE is not in this build", dave);
        dv_emit(DV_EVT_DAVE_REQUIRED, "session description: dave_protocol_version > 0");
        return false;
    }
    uint8_t key[32];
    int i = 0;
    const cJSON *b;
    cJSON_ArrayForEach(b, cJSON_GetObjectItemCaseSensitive(d, "secret_key"))
        if (i < 32 && cJSON_IsNumber(b)) key[i++] = (uint8_t)b->valueint;
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_ENCRYPT);
    psa_set_key_algorithm(&a, PSA_ALG_GCM);
    psa_set_key_type(&a, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&a, 256);
    xSemaphoreTake(L.mx, portMAX_DELAY);
    psa_status_t st = i == 32 ? psa_import_key(&a, key, 32, &L.key) : PSA_ERROR_INVALID_ARGUMENT;
    bool ok = st == PSA_SUCCESS && open_opus();
    memset(key, 0, sizeof key);
    L.seq = (uint16_t)esp_random();
    L.ts = esp_random();
    L.nonce = 0;
    L.speaking = false;
    L.ready = ok;
    xSemaphoreGive(L.mx);
    if (!ok) {
        ESP_LOGE(TAG, "key import %d (%d key bytes) or opus failed", (int)st, i);
        dv_emit(DV_EVT_ERROR, "voice: key or opus setup failed");
        return false;
    }
    dv_set_state("ready");
    ESP_LOGI(TAG, "voice ready (mode " MODE ", dave %d)", dave);
    dv_emit(DV_EVT_VOICE_READY, s_creds.endpoint);
    return true;
}

/* One voice connection, until it closes or the gateway stops or restarts us.
 * Returns the close code (0: we stopped it or never got one). */
static int run_link(char *buf)
{
    char host[128];
    int port = 443;
    snprintf(host, sizeof host, "%s", s_creds.endpoint);
    char *colon = strrchr(host, ':');
    if (colon) { port = atoi(colon + 1); *colon = 0; }
    dv_set_state("voice connect");
    dv_ws_t *ws = dv_ws_open(host, port, "/?v=8", 10000);
    if (!ws) { dv_emit(DV_EVT_ERROR, "voice: cannot connect"); return 0; }
    v_identify(ws);
    int hb_ms = 13750, seq = -1, code = 0;
    int64_t next_hb = 0, next_keepalive = 0;
    uint32_t keepalive = 0;
    while (!s_stop && !s_begin) {
        int64_t now = esp_timer_get_time() / 1000;
        int wait = 50;                                 /* wake often for the speaking flag */
        if (next_hb && next_hb - now < wait) wait = next_hb > now ? (int)(next_hb - now) : 0;
        int want = L.speaking_want;
        if (want >= 0) { L.speaking_want = -1; v_speaking(ws, want); }
        bool bin = false;
        int n = dv_ws_recv(ws, buf, VBUF, wait, &bin);
        if (n < 0) {
            const char *why = "";
            code = dv_ws_close_code(ws, &why);
            char t[160];
            snprintf(t, sizeof t, "voice closed: %d %s %s", code, close_name(code), why);
            dv_emit(code == 4017 ? DV_EVT_DAVE_REQUIRED : DV_EVT_ERROR, t);
            break;
        }
        now = esp_timer_get_time() / 1000;
        if (next_hb && now >= next_hb) {
            char hb[96];
            snprintf(hb, sizeof hb, "{\"op\":3,\"d\":{\"t\":%lld,\"seq_ack\":%d}}", (long long)now, seq);
            dv_ws_send_text(ws, hb);
            next_hb = now + hb_ms;
        }
        if (L.ready && now >= next_keepalive) {        /* keep the NAT mapping while quiet, as discord.js */
            uint8_t ka[8] = { keepalive, keepalive >> 8, keepalive >> 16, keepalive >> 24 };
            keepalive++;
            sendto(L.udp, ka, sizeof ka, 0, (struct sockaddr *)&L.dest, sizeof L.dest);
            next_keepalive = now + 5000;
        }
        if (n == 0) continue;
        if (bin) {                                     /* [seq u16][op u8][payload]: DAVE's MLS messages */
            ESP_LOGW(TAG, "binary voice message, op %d, ignored (no DAVE)", n > 2 ? (uint8_t)buf[2] : -1);
            continue;
        }
        cJSON *msg = cJSON_ParseWithLength(buf, n);
        if (!msg) continue;
        const cJSON *d = cJSON_GetObjectItemCaseSensitive(msg, "d");
        seq = jint(msg, "seq", seq);
        int op = jint(msg, "op", -1);
        bool keep = true;
        switch (op) {
        case 8:                                        /* Hello */
            hb_ms = jint(d, "heartbeat_interval", hb_ms);
            next_hb = now + hb_ms;
            break;
        case 2: keep = on_ready(ws, d); break;         /* Ready */
        case 4: keep = on_session(d); break;           /* Session Description */
        case 6: break;                                 /* heartbeat ACK */
        default:
            if (op >= 21 && op <= 31) ESP_LOGW(TAG, "DAVE op %d from the server (transition?)", op);
            else ESP_LOGD(TAG, "voice op %d", op);
            break;
        }
        cJSON_Delete(msg);
        if (!keep) break;
    }
    teardown();
    dv_ws_close(ws);
    return code;
}

static void voice_task(void *arg)
{
    (void)arg;
    char *buf = heap_caps_malloc(VBUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { dv_emit(DV_EVT_ERROR, "voice: no memory"); s_task = NULL; vTaskDeleteWithCaps(NULL); }
    for (;;) {
        while (!s_begin) vTaskDelay(pdMS_TO_TICKS(100));
        s_begin = s_stop = false;
        /* a dropped link is retried with the same session (crash, timeout), up to three times;
         * kicked, bad auth or DAVE-required are final until the gateway sends new credentials */
        for (int attempt = 0; attempt < 3 && !s_stop && !s_begin; attempt++) {
            int code = run_link(buf);
            if (s_stop || s_begin) break;
            if (code == 4004 || code == 4006 || code == 4014 || code == 4017 || code == 4022) break;
            vTaskDelay(pdMS_TO_TICKS(2000 << attempt));
        }
        if (!s_begin) {
            dv_set_state("voice down");
            dv_emit(DV_EVT_DISCONNECTED, "voice");
        }
    }
}

void dv_voice_begin(const dv_voice_creds_t *creds)
{
    if (!L.mx) {
        L.mx = xSemaphoreCreateMutex();
        psa_crypto_init();
    }
    s_creds = *creds;
    s_begin = true;                                /* a running link restarts with the new creds */
    if (!s_task) xTaskCreateWithCaps(voice_task, "dv_voice", 8192, NULL, 6, &s_task, MALLOC_CAP_SPIRAM);
}

void dv_voice_stop(void)
{
    s_stop = true;
}
