/*
 * dbk.c — DBK link v1, receive side + point-to-point ack + status sync (see dbk.h).
 *
 * Nothing here touches the BLE heartbeat, OTA, Improv or voice: the radio is started through
 * wifi_radio_start() (no join without a saved network), and the channel is only changed while
 * the STA is not associated and no scan or join owns the radio (wifi_assoc() / wifi_busy()).
 */
#include "dbk.h"
#include "wifi.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "dbk";

#define FRAME_MAX     250                 /* ESP-NOW v1 payload limit */
#define STATUS_US     (5 * 1000000LL)     /* our status broadcast period */
#define DWELL_US      (5500 * 1000LL)     /* > the kit's 5 s send period */
#define SILENCE_US    (30 * 1000000LL)    /* re-hop after this long without a dbk frame */
#define LIVE_US       (60 * 1000000LL)    /* a kit reading is shown this long */
#define PEER_LIVE_US  (30 * 1000000LL)    /* a pet/kit counts as "here" this long */
#define LOG_US        (5 * 1000000LL)     /* one log line per kind per 5 s */
#define MAX_PEERS     8
#define BOTH_US       (10 * 1000000LL)    /* "EB" when both sources were heard this recently */
#define PM_MAX        65535               /* a PM value outside 0..PM_MAX drops the frame */

static const uint8_t BCAST[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
static const uint8_t HOP[]    = { 1, 6, 11, 2, 3, 4, 5, 7, 8, 9, 10, 12, 13 };

typedef struct {
    uint8_t src[6];
    int8_t  rssi;
    uint8_t ch;
    uint8_t len;
    char    data[FRAME_MAX + 1];
} rx_item_t;

typedef struct {
    bool    used;
    char    id[8];
    int     pm25, pm10, pm1, band, rssi;
    int64_t t_us;                         /* latest reading, any source */
    int64_t t_e, t_b;                     /* latest ESP-NOW / BLE reading (0 = never) */
} kit_t;

typedef struct {
    bool    used;
    bool    is_kit;
    char    id[8];
    char    name[20];
    char    state[12];
    char    kit[8];
    uint8_t mac[6];
    int     ch;
    int64_t t_us;
} peer_t;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static kit_t        s_kits[DBK_MAX_KITS];
static peer_t       s_peers[MAX_PEERS];
static uint32_t     s_rx;
static volatile int s_ch;
static volatile bool s_locked;
static char         s_state[12] = "idle";
static char         s_pack[24]  = "";
static char         s_name[20]  = "Claude-0000";
static char         s_myid[8]   = "0000";
static QueueHandle_t s_q;
static bool         s_ready;

/* the latest BLE reading, logged by the task (the scan callback stays quiet) */
static volatile bool s_ble_new;
static dbk_kit_view_t s_ble_last;

int dbk_band_of(int pm25) { return pm25 <= 25 ? DBK_BAND_GOOD : pm25 <= 50 ? DBK_BAND_FAIR : DBK_BAND_POOR; }

const char *dbk_band_name(int band)
{
    return band == DBK_BAND_GOOD ? "good" : band == DBK_BAND_FAIR ? "fair" : "poor";
}

/* ---- tiny flat-JSON reader (the frames are one level, ASCII, <= 250 bytes) ---- */
static const char *jval(const char *s, const char *key)
{
    char pat[16];
    int n = snprintf(pat, sizeof pat, "\"%s\"", key);
    if (n <= 0 || n >= (int)sizeof pat) return NULL;
    /* a key is "name" right after '{' or ',' and followed by ':' -- a string VALUE equal to a
     * key name (a pack called "ch") is skipped, not mistaken for the key */
    for (const char *p = strstr(s, pat); p; p = strstr(p + 1, pat)) {
        const char *b = p;
        while (b > s && b[-1] == ' ') b--;
        if (b == s || (b[-1] != '{' && b[-1] != ',')) continue;
        const char *q = p + n;
        while (*q == ' ') q++;
        if (*q != ':') continue;
        q++;
        while (*q == ' ') q++;
        return q;
    }
    return NULL;
}

static bool jint(const char *s, const char *key, long *out)
{
    const char *p = jval(s, key);
    if (!p || !(*p == '-' || (*p >= '0' && *p <= '9'))) return false;
    *out = strtol(p, NULL, 10);
    return true;
}

static bool jstr(const char *s, const char *key, char *out, size_t n)
{
    const char *p = jval(s, key);
    if (!p || *p != '"' || n == 0) return false;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < n) out[i++] = *p++;
    out[i] = 0;
    return *p == '"';
}

static void lower(char *s) { for (; *s; s++) if (*s >= 'A' && *s <= 'Z') *s += 32; }

/* ---- tables (caller holds s_mux) ---- */
static void kit_put(const char *id, int pm25, int pm10, int pm1, int band, int rssi, char src, int64_t now)
{
    kit_t *slot = NULL, *oldest = &s_kits[0];
    for (int i = 0; i < DBK_MAX_KITS; i++) {
        if (s_kits[i].used && strcmp(s_kits[i].id, id) == 0) { slot = &s_kits[i]; break; }
        if (!s_kits[i].used && !slot) slot = &s_kits[i];
        if (s_kits[i].t_us < oldest->t_us) oldest = &s_kits[i];
    }
    if (!slot) slot = oldest;
    if (!slot->used || strcmp(slot->id, id) != 0) { memset(slot, 0, sizeof *slot); strlcpy(slot->id, id, sizeof slot->id); }
    slot->used = true;
    slot->pm25 = pm25; slot->pm10 = pm10; slot->pm1 = pm1;
    slot->band = (band >= 0 && band <= 2) ? band : dbk_band_of(pm25);
    slot->rssi = rssi; slot->t_us = now;
    if (src == DBK_SRC_BLE) slot->t_b = now; else slot->t_e = now;
}

static peer_t *peer_put(const char *id, bool is_kit, const uint8_t *mac, int64_t now)
{
    peer_t *slot = NULL, *oldest = &s_peers[0];
    for (int i = 0; i < MAX_PEERS; i++) {
        if (s_peers[i].used && s_peers[i].is_kit == is_kit && strcmp(s_peers[i].id, id) == 0) { slot = &s_peers[i]; break; }
        if (!s_peers[i].used && !slot) slot = &s_peers[i];
        if (s_peers[i].t_us < oldest->t_us) oldest = &s_peers[i];
    }
    if (!slot) { slot = oldest; memset(slot, 0, sizeof *slot); }
    slot->used = true;
    slot->is_kit = is_kit;
    strlcpy(slot->id, id, sizeof slot->id);
    if (mac) memcpy(slot->mac, mac, 6);
    slot->t_us = now;
    return slot;
}

/* ---- public, any task ---- */
void dbk_on_reading(const char *id, int pm25, int pm10, int pm1, int band, int rssi, char src)
{
    if (!id || !id[0] || pm25 < 0 || pm25 > PM_MAX) return;
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_mux);
    kit_put(id, pm25, pm10, pm1, band, rssi, src, now);
    peer_put(id, true, NULL, now);
    if (src == DBK_SRC_BLE) {
        strlcpy(s_ble_last.id, id, sizeof s_ble_last.id);
        s_ble_last.pm25 = pm25; s_ble_last.pm10 = pm10; s_ble_last.pm1 = pm1;
        s_ble_last.band = band; s_ble_last.rssi = rssi;
        s_ble_new = true;
    }
    portEXIT_CRITICAL(&s_mux);
}

void dbk_set_state(const char *state)
{
    portENTER_CRITICAL(&s_mux);
    strlcpy(s_state, state ? state : "idle", sizeof s_state);
    portEXIT_CRITICAL(&s_mux);
}

void dbk_set_pack(const char *pack)
{
    portENTER_CRITICAL(&s_mux);
    strlcpy(s_pack, pack ? pack : "", sizeof s_pack);
    portEXIT_CRITICAL(&s_mux);
}

void dbk_view(dbk_view_t *out)
{
    memset(out, 0, sizeof *out);
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < DBK_MAX_KITS; i++) {
        const kit_t *k = &s_kits[i];
        if (!k->used || now - k->t_us > LIVE_US) continue;
        dbk_kit_view_t v = { .pm25 = k->pm25, .pm10 = k->pm10, .pm1 = k->pm1, .band = k->band,
                             .rssi = k->rssi, .age_us = now - k->t_us };
        strlcpy(v.id, k->id, sizeof v.id);
        /* a stable source tag: "EB" while both are heard, so the HUD does not flip E<->B */
        bool e = k->t_e && now - k->t_e < BOTH_US, b = k->t_b && now - k->t_b < BOTH_US;
        strlcpy(v.src, e && b ? "EB" : e ? "E" : b ? "B" : (k->t_e > k->t_b ? "E" : "B"), sizeof v.src);
        int j = out->nkits++;                       /* insertion sort, freshest first */
        while (j > 0 && out->kit[j - 1].age_us > v.age_us) { out->kit[j] = out->kit[j - 1]; j--; }
        out->kit[j] = v;
    }
    for (int i = 0; i < MAX_PEERS; i++) {
        const peer_t *p = &s_peers[i];
        if (!p->used || now - p->t_us > PEER_LIVE_US) continue;
        if (p->is_kit) out->kit_ok = true; else out->pets++;
    }
    out->rx = s_rx;
    portEXIT_CRITICAL(&s_mux);
    out->ch = s_ch;
    out->locked = s_locked;
}

/* ---- ESP-NOW callbacks: WiFi task, copy only ---- */
static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (!s_q || !info || !data || len <= 0 || len > FRAME_MAX || data[0] != '{') return;
    rx_item_t it;
    memcpy(it.src, info->src_addr, 6);
    it.rssi = info->rx_ctrl ? info->rx_ctrl->rssi : 0;
    it.ch   = info->rx_ctrl ? info->rx_ctrl->channel : 0;
    it.len  = (uint8_t)len;
    memcpy(it.data, data, len);
    it.data[len] = 0;
    xQueueSend(s_q, &it, 0);                        /* full: drop, the next one is 5 s away */
}

static volatile int     s_ack_ok, s_ack_fail;
static void on_sent(const esp_now_send_info_t *tx, esp_now_send_status_t st)
{
    if (!tx || !tx->des_addr || memcmp(tx->des_addr, BCAST, 6) == 0) return;
    if (st == ESP_NOW_SEND_SUCCESS) s_ack_ok++; else s_ack_fail++;   /* MAC-layer ACK = delivered */
}

/* ---- task side ---- */
static bool set_channel(int ch)
{
    if (ch < 1 || ch > 13) return false;
    if (esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE) != ESP_OK) return false;
    s_ch = ch;
    return true;
}

static void ensure_peer(const uint8_t mac[6])
{
    static uint8_t warned[4][6];                    /* "table full" once per MAC, not every 5 s */
    static int     warned_next;
    if (esp_now_is_peer_exist(mac)) return;
    esp_now_peer_info_t p = { 0 };
    memcpy(p.peer_addr, mac, 6);
    p.channel = 0;                                  /* 0 = whatever channel we are on now */
    p.ifidx   = WIFI_IF_STA;
    p.encrypt = false;
    esp_err_t e = esp_now_add_peer(&p);
    if (e == ESP_ERR_ESPNOW_FULL) {
        for (int i = 0; i < 4; i++) if (memcmp(warned[i], mac, 6) == 0) return;
        memcpy(warned[warned_next], mac, 6);
        warned_next = (warned_next + 1) % 4;
        ESP_LOGW(TAG, "peer table full (20), no unicast to " MACSTR, MAC2STR(mac));
    }
    else if (e != ESP_OK) ESP_LOGW(TAG, "add peer " MACSTR ": %s", MAC2STR(mac), esp_err_to_name(e));
}

static bool pm_ok(long v) { return v >= 0 && v <= PM_MAX; }

/* true = a valid "dbk" reading (the only thing that keeps the channel lock alive) */
static bool handle(const rx_item_t *it, int64_t now)
{
    static int64_t log_dbk_us, log_pet_us, log_ack_us;
    char t[8] = "", id[8] = "";
    if (!jstr(it->data, "t", t, sizeof t) || !jstr(it->data, "id", id, sizeof id) || !id[0]) return false;
    lower(id);
    long v = 0;
    if (jint(it->data, "v", &v) && v != 1) return false;

    if (strcmp(t, "dbk") == 0) {
        long pm25 = -1, pm10 = -1, pm1 = -1, seq = -1, kch = 0;
        if (!jint(it->data, "pm25", &pm25) || !pm_ok(pm25)) return false;
        if (jint(it->data, "pm10", &pm10) && !pm_ok(pm10)) return false;
        if (jint(it->data, "pm1", &pm1) && !pm_ok(pm1)) return false;
        char band[8] = "", to[8] = "";
        jstr(it->data, "band", band, sizeof band);
        int b = !strcmp(band, "good") ? DBK_BAND_GOOD : !strcmp(band, "fair") ? DBK_BAND_FAIR
              : !strcmp(band, "poor") ? DBK_BAND_POOR : dbk_band_of((int)pm25);
        bool unicast = jstr(it->data, "to", to, sizeof to);
        lower(to);
        portENTER_CRITICAL(&s_mux);
        kit_put(id, (int)pm25, (int)pm10, (int)pm1, b, it->rssi, DBK_SRC_ESPNOW, now);
        peer_t *p = peer_put(id, true, it->src, now);
        p->ch = it->ch;
        s_rx++;
        portEXIT_CRITICAL(&s_mux);

        /* tune to the kit's channel: its own "ch" if it sends one, else the rx channel. A frame
         * caught during a join scan or leaking in from an adjacent channel would otherwise lock
         * the pet on the wrong one. Never while joined or while a scan/join owns the radio. */
        int want = (jint(it->data, "ch", &kch) && kch >= 1 && kch <= 13) ? (int)kch : it->ch;
        if (!wifi_assoc() && !wifi_busy() && want >= 1 && want <= 13 && want != s_ch) {
            int was = s_ch;
            if (set_channel(want)) ESP_LOGI(TAG, "kit %s is on ch=%d: moved from ch=%d", id, want, was);
        }
        if (!s_locked) ESP_LOGI(TAG, "lock ch=%d on kit %s", s_ch, id);
        s_locked = true;

        if (unicast && strcmp(to, s_myid) == 0 && jint(it->data, "seq", &seq)) {
            ensure_peer(it->src);
            char ack[64];
            int n = snprintf(ack, sizeof ack, "{\"t\":\"ack\",\"v\":1,\"id\":\"%s\",\"seq\":%ld}", s_myid, seq);
            esp_err_t e = esp_now_send(it->src, (const uint8_t *)ack, n);
            if (e != ESP_OK && now - log_ack_us >= LOG_US) {
                log_ack_us = now;
                ESP_LOGW(TAG, "ack seq=%ld to %s: %s", seq, id, esp_err_to_name(e));
            }
        }
        if (now - log_dbk_us >= LOG_US) {
            log_dbk_us = now;
            ESP_LOGI(TAG, "rx espnow ch=%d id=%s pm25=%ld pm10=%ld pm1=%ld band=%s rssi=%d %s%s seq=%ld acks=%d/%d",
                     it->ch, id, pm25, pm10, pm1, dbk_band_name(b), it->rssi,
                     unicast ? "to=" : "bcast", unicast ? to : "", seq, s_ack_ok, s_ack_ok + s_ack_fail);
        }
        return true;
    } else if (strcmp(t, "pet") == 0) {
        if (strcmp(id, s_myid) == 0) return false;
        char name[20] = "", state[12] = "", kit[8] = "";
        long ch = 0;
        jstr(it->data, "name", name, sizeof name);
        jstr(it->data, "state", state, sizeof state);
        jstr(it->data, "kit", kit, sizeof kit);
        jint(it->data, "ch", &ch);
        portENTER_CRITICAL(&s_mux);
        peer_t *p = peer_put(id, false, it->src, now);
        strlcpy(p->name, name, sizeof p->name);
        strlcpy(p->state, state, sizeof p->state);
        strlcpy(p->kit, kit, sizeof p->kit);
        p->ch = (int)ch;
        portEXIT_CRITICAL(&s_mux);
        if (now - log_pet_us >= LOG_US) {
            log_pet_us = now;
            ESP_LOGI(TAG, "rx pet ch=%d id=%s name=%s state=%s kit=%s rssi=%d", it->ch, id, name, state, kit, it->rssi);
        }
    }
    /* "ack" frames are for kits; a pet ignores them */
    return false;
}

static void send_status(void)
{
    char state[12], pack[24], kit[8] = "";
    uint32_t rx;
    int64_t now = esp_timer_get_time(), best = -1;
    portENTER_CRITICAL(&s_mux);
    strlcpy(state, s_state, sizeof state);
    strlcpy(pack, s_pack, sizeof pack);
    rx = s_rx;
    for (int i = 0; i < DBK_MAX_KITS; i++)
        if (s_kits[i].used && now - s_kits[i].t_us < PEER_LIVE_US && s_kits[i].t_us > best) {
            best = s_kits[i].t_us;
            strlcpy(kit, s_kits[i].id, sizeof kit);
        }
    portEXIT_CRITICAL(&s_mux);
    char f[FRAME_MAX];
    int n = snprintf(f, sizeof f,
                     "{\"t\":\"pet\",\"v\":1,\"id\":\"%s\",\"name\":\"%s\",\"state\":\"%s\",\"pack\":\"%s\","
                     "\"rx\":%" PRIu32 ",\"kit\":\"%s\",\"ch\":%d}",
                     s_myid, s_name, state, pack, rx, kit, s_ch);
    if (n > 0 && n < (int)sizeof f) esp_now_send(BCAST, (const uint8_t *)f, n);
}

/* The channel the radio is really on (a scan or a join leaves it wherever it ended). */
static int radio_channel(void)
{
    uint8_t ch = 0;
    wifi_second_chan_t sec;
    return esp_wifi_get_channel(&ch, &sec) == ESP_OK ? ch : 0;
}

/* NOTE: this task's stack is in PSRAM (dbk_init). That is safe only while nothing here writes
 * flash (NVS, LittleFS): the cache is off during a flash write and a PSRAM stack would crash.
 * WiFi storage is RAM-only (wifi.c), so the WiFi / ESP-NOW calls below never write flash.
 * Anything that must save to NVS from here has to hand off to another task first. */
static void dbk_task(void *arg)
{
    (void)arg;
    rx_item_t it;
    int64_t last_status = 0, dwell_start = 0, last_dbk = 0;
    int hop = 0;
    bool assoc_shown = false, was_busy = false, slow = false;
    if (!wifi_assoc() && !wifi_busy()) set_channel(HOP[0]);
    else s_ch = radio_channel();
    dwell_start = esp_timer_get_time();
    for (;;) {
        if (xQueueReceive(s_q, &it, pdMS_TO_TICKS(100)) == pdTRUE) {
            int64_t t = esp_timer_get_time();
            if (handle(&it, t)) last_dbk = t;       /* only a valid dbk reading keeps the lock */
        }
        int64_t now = esp_timer_get_time();

        if (s_ble_new) {
            static int64_t log_ble_us;
            dbk_kit_view_t b;
            portENTER_CRITICAL(&s_mux);
            b = s_ble_last;
            s_ble_new = false;
            portEXIT_CRITICAL(&s_mux);
            if (now - log_ble_us >= LOG_US) {
                log_ble_us = now;
                ESP_LOGI(TAG, "rx ble id=%s pm25=%d pm10=%d pm1=%d band=%s rssi=%d",
                         b.id, b.pm25, b.pm10, b.pm1, dbk_band_name(b.band), b.rssi);
            }
        }

        /* channel: the AP's when associated (never hop), else lock on a kit or keep hopping.
         * Never set the channel while a scan or a join owns the radio (wifi_busy()). */
        bool busy = wifi_busy();
        if (wifi_assoc()) {
            int ch = radio_channel();
            if (ch) s_ch = ch;
            s_locked = true;
            if (!assoc_shown) { ESP_LOGI(TAG, "associated: listening on AP ch=%d, no hopping", s_ch); assoc_shown = true; }
        } else {
            if (assoc_shown) {
                assoc_shown = false;
                if (last_dbk == 0 || now - last_dbk > SILENCE_US) {
                    s_locked = false;
                    dwell_start = now;
                    ESP_LOGI(TAG, "left the AP (ch=%d), no kit heard: hopping", s_ch);
                }
            }
            if (s_locked && (last_dbk == 0 || now - last_dbk > SILENCE_US)) {
                s_locked = false;
                dwell_start = now;
                if (last_dbk) ESP_LOGI(TAG, "no kit for 30 s on ch=%d: hopping", s_ch);
            }
            if (!busy) {
                /* a scan or a join that just ended (or anything else) left the radio elsewhere:
                 * put it back on our channel, so a locked pet really listens where it says */
                int ch = radio_channel();
                if (ch && s_ch && ch != s_ch) {
                    int want = s_ch;
                    if (set_channel(want) && was_busy) ESP_LOGI(TAG, "radio back on ch=%d (was ch=%d after scan/join)", want, ch);
                }
                if (!s_locked && now - dwell_start >= DWELL_US) {
                    int next = (hop + 1) % (int)sizeof HOP;
                    if (set_channel(HOP[next])) { hop = next; dwell_start = now; }   /* failed: try again next pass */
                }
            }
        }
        was_busy = busy;

        /* locked on a kit while not joined: retry the saved network slowly (60 s), so the join
         * sweep does not keep pulling the radio off the kit's channel */
        bool want_slow = s_locked && !wifi_assoc();
        if (want_slow != slow) { slow = want_slow; wifi_set_slow_retry(slow); }

        if (now - last_status >= STATUS_US && !busy) {
            last_status = now;
            send_status();
        }
    }
}

void dbk_init(const char *name, const char *pack)
{
    if (s_ready) return;
    if (name) strlcpy(s_name, name, sizeof s_name);
    if (pack) strlcpy(s_pack, pack, sizeof s_pack);
    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK)
        snprintf(s_myid, sizeof s_myid, "%02x%02x", mac[4], mac[5]);

    if (!wifi_radio_start()) { ESP_LOGE(TAG, "radio did not start: DBK link off"); return; }
    esp_err_t e = esp_wifi_set_ps(WIFI_PS_NONE);   /* modem sleep misses broadcasts */
    if (e != ESP_OK) ESP_LOGW(TAG, "power save still on (%s): broadcasts will be missed", esp_err_to_name(e));
    e = esp_now_init();
    if (e != ESP_OK) { ESP_LOGE(TAG, "esp_now_init: %s", esp_err_to_name(e)); return; }
    s_q = xQueueCreate(8, sizeof(rx_item_t));
    if (!s_q) { ESP_LOGE(TAG, "no queue"); esp_now_deinit(); return; }
    esp_now_register_recv_cb(on_recv);
    esp_now_register_send_cb(on_sent);
    ensure_peer(BCAST);                             /* broadcast sends need the peer too */
    if (xTaskCreateWithCaps(dbk_task, "dbk", 4096, NULL, 3, NULL, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "no task");
        return;
    }
    s_ready = true;
    ESP_LOGI(TAG, "DBK link v1 up: id=%s name=%s (ESP-NOW + BLE 'DB')", s_myid, s_name);
}
