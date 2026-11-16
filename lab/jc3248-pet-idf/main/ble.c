/*
 * ble.c — jc3248-pet-idf STEP 9: BLE NUS bridge on ESP-IDF v6 NimBLE.
 *
 * Two halves, mirroring the Arduino ble.cpp split:
 *
 *   1) TRANSPORT (NimBLE host) — GATT server with the NUS service, an open RX
 *      WRITE char whose callback appends raw bytes into a shared ring under a
 *      portMUX critical section, an (advertised, future-use) TX NOTIFY char, and
 *      GAP advertising as "Claude-XXXX". Skeleton mirrors the IDF example
 *      examples/bluetooth/nimble/ble_spp/spp_server. Open / no-bond.
 *
 *   2) PARSER (main thread) — BleSnapshot + the line-assembly buffer + blePoll()
 *      drain/scan/parse + parseLine(). Ported VERBATIM from ble.cpp. The only
 *      change is ArduinoJson -> a tiny flat-JSON scanner (the same dependency-
 *      free strstr/copy approach gif.cpp::manifest_parse already uses here), so
 *      no `json` component is pulled in.
 *
 * Threading note (matches the Arduino contract): the RX callback runs on the
 * NimBLE host task and ONLY appends bytes under rxMux. blePoll() runs on the
 * main task, copies bytes out under the same lock, then assembles + parses lines
 * OUTSIDE the lock — so the (slightly heavier) scanner never runs on the host
 * task. portMUX_TYPE critical sections are valid on both tasks for this purpose.
 */
#include "ble.h"
#include "improv.h"
#include "ota.h"   /* STEP 11: firmware update service next to NUS */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "nvs_flash.h"

/* NimBLE */
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "host/ble_uuid.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs_adv.h"   /* ble_hs_adv_parse_fields — read the time-beacon name */
#include "esp_timer.h"
#include "stand.h"            /* bot stand: other pets heard by the scan */
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "ble";

/* ===========================================================================
 * NUS UUIDs (de-facto serial-over-BLE standard).
 * 128-bit, little-endian byte order for NimBLE's BLE_UUID128_INIT (LSB first).
 * 6e400001-b5a3-f393-e0a9-e50e24dcca9e  ->  ca,9e ... 6e,40,00,01 reversed.
 * ===========================================================================*/
/* Service: 6e400001-b5a3-f393-e0a9-e50e24dcca9e */
static const ble_uuid128_t NUS_SVC_UUID =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);
/* RX char (client -> device, WRITE): 6e400002-... */
static const ble_uuid128_t NUS_RX_UUID =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);
/* TX char (device -> client, NOTIFY): 6e400003-... */
static const ble_uuid128_t NUS_TX_UUID =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);

/* ===========================================================================
 * Shared RX byte buffer (ported from ble.cpp).
 *
 * The RX write callback (NimBLE host task) only appends raw bytes here under
 * rxMux. blePoll() (main task) drains the buffer under the same lock, copy only,
 * then scans/parses lines OUTSIDE the lock so the scanner never runs on the host
 * task. Cap 1KB; one heartbeat line is well under that. On overflow (a peer that
 * never sends '\n') we reset rather than block the host task.
 * ===========================================================================*/
#define RX_CAP 1024
static portMUX_TYPE    rxMux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t         rxBuf[RX_CAP];
static volatile size_t rxLen = 0;

/* Line-assembly buffer owned exclusively by blePoll() / the main thread. */
static char   lineBuf[RX_CAP];
static size_t lineLen = 0;

static volatile bool   connected = false;
static bool            firstSnapshotLogged = false;

/* Time broadcast (observer role): the host advertises a BLE local name
 * "clk:<epoch>:<tzoff>". The scan callback parses it here; blePollBroadcastTime()
 * drains it on the main thread. No connection involved. */
static volatile long   s_bcast_epoch = -1;
static volatile long   s_bcast_tzoff = 0;
static volatile bool   s_bcast_new   = false;

/* GATT/GAP state. */
static char            s_devName[32] = "Claude-0000";
static uint8_t         s_own_addr_type;
static uint16_t        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t        s_tx_val_handle;       /* TX notify char value handle    */
static bool            s_tx_subscribed = false;

static void ble_advertise(void);
static void ble_scan_start(void);

bool bleNotifyLine(const char *json)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || !s_tx_subscribed || !json) return false;
    char line[200];
    int n = snprintf(line, sizeof(line), "%s\n", json);
    if (n <= 0 || n >= (int)sizeof(line)) return false;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(line, (uint16_t)n);
    if (!om) return false;
    int rc = ble_gatts_notify_custom(s_conn_handle, s_tx_val_handle, om);   /* consumes om */
    if (rc != 0) ESP_LOGW(TAG, "notify failed rc=%d", rc);
    return rc == 0;
}

/* ---------------------------------------------------------------------------
 * GATT access callback — RX WRITE. Host task. Keep tiny: append bytes under the
 * critical section (mirrors RxCallbacks::onWrite). Reads are unused (NUS RX is
 * write-only) but handled defensively. The TX char is notify-only — no access cb
 * body needed; subscription is tracked in the GAP SUBSCRIBE event.
 * ------------------------------------------------------------------------- */
static int nus_rx_access(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return 0;
    }
    uint16_t n = OS_MBUF_PKTLEN(ctxt->om);
    if (n == 0) return 0;

    /* Flatten the incoming mbuf into a stack scratch (capped at RX_CAP). */
    uint8_t tmp[RX_CAP];
    uint16_t got = 0;
    int rc = ble_hs_mbuf_to_flat(ctxt->om, tmp, sizeof(tmp), &got);
    if (rc != 0 || got == 0) return 0;

    portENTER_CRITICAL(&rxMux);
    for (uint16_t i = 0; i < got; i++) {
        if (rxLen >= RX_CAP) { rxLen = 0; }   /* overflow: drop, resync on '\n' */
        rxBuf[rxLen++] = tmp[i];
    }
    portEXIT_CRITICAL(&rxMux);
    return 0;
}

/* TX char access cb — notify-only, never read/written directly. Present so the
 * stack has a handler; returns 0 (read returns empty). */
static int nus_tx_access(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    return 0;
}

/* ---------------------------------------------------------------------------
 * NUS GATT service definition: one primary service, RX (write) + TX (notify).
 * Mirrors the spp_server ble_gatt_svc_def[] shape with the NUS 128-bit UUIDs.
 * Open chars: no _ENC / _AUTHEN flags, so no pairing required.
 * ------------------------------------------------------------------------- */
static const struct ble_gatt_svc_def nus_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &NUS_SVC_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                /* RX: client -> device, open WRITE (+ write no-rsp). */
                .uuid = &NUS_RX_UUID.u,
                .access_cb = nus_rx_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                /* TX: device -> client, NOTIFY (+ READ for completeness).
                 * NimBLE auto-adds the CCCD for a NOTIFY char. */
                .uuid = &NUS_TX_UUID.u,
                .access_cb = nus_tx_access,
                .val_handle = &s_tx_val_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            },
            { 0 }   /* no more characteristics */
        },
    },
    { 0 }   /* no more services */
};

/* ---------------------------------------------------------------------------
 * GAP event handler — connection state + re-advertise on disconnect (mirrors
 * ServerCallbacks). Tracks TX subscription for future notify use.
 * ------------------------------------------------------------------------- */
static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            connected = true;
            s_conn_handle = event->connect.conn_handle;
            ota_on_connect(s_conn_handle);
            ble_gap_disc_cancel();      /* pause scan: don't radio-starve the link */
            ESP_LOGI(TAG, "connected (conn=%d) — scan paused", s_conn_handle);
        } else {
            ESP_LOGW(TAG, "connect failed; status=%d — re-advertising",
                     event->connect.status);
            ble_advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        connected = false;
        s_tx_subscribed = false;
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        ota_on_disconnect();            /* an update in flight is abandoned; old app keeps running */
        improv_on_disconnect();
        ESP_LOGI(TAG, "disconnected; reason=%d — re-advertising + resuming scan",
                 event->disconnect.reason);
        ble_advertise();
        ble_scan_start();               /* resume scanning for the time broadcast */
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ble_advertise();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_tx_val_handle) {
            s_tx_subscribed = event->subscribe.cur_notify;
        }
        ota_on_subscribe(event->subscribe.attr_handle, event->subscribe.cur_notify);
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "mtu=%d (conn=%d)", event->mtu.value,
                 event->mtu.conn_handle);
        return 0;

    default:
        return 0;
    }
}

/* ---------------------------------------------------------------------------
 * Advertising: general discoverable, undirected connectable, name + NUS svc
 * UUID in the scan response (the 128-bit UUID is large, so split name in the
 * adv packet and the service UUID in the scan-rsp to fit 31 bytes each).
 * ------------------------------------------------------------------------- */
static void ble_advertise(void)
{
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;
    struct ble_hs_adv_fields rsp_fields;
    int rc;

    /* Advertisement (31 bytes, full): flags 3 + the Improv service UUID 18 + Improv service
     * data 10 (UUID 0x4677: state, capabilities, 4 reserved), so Improv clients find the pet.
     * Scan response (31 bytes, full): the NUS UUID 18 + the name 13, read by OraclePet and the
     * libraries (active scanning merges both). */
    memset(&fields, 0, sizeof fields);
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = (ble_uuid128_t *)improv_uuid();
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 0;
    static uint8_t svc_data[8];
    svc_data[0] = 0x77; svc_data[1] = 0x46;          /* 0x4677, little-endian */
    svc_data[2] = improv_state(); svc_data[3] = 0x01; /* capabilities: identify */
    fields.svc_data_uuid16 = svc_data;
    fields.svc_data_uuid16_len = sizeof svc_data;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields failed; rc=%d", rc);
        return;
    }

    /* 128-bit NUS service UUID in the scan response (too big for adv + name). */
    memset(&rsp_fields, 0, sizeof rsp_fields);
    rsp_fields.uuids128 = (ble_uuid128_t *)&NUS_SVC_UUID;
    rsp_fields.num_uuids128 = 1;
    rsp_fields.uuids128_is_complete = 0;
    rsp_fields.name = (uint8_t *)s_devName;
    rsp_fields.name_len = strlen(s_devName);
    rsp_fields.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        ESP_LOGW(TAG, "adv_rsp_set_fields failed; rc=%d (continuing)", rc);
    }

    memset(&adv_params, 0, sizeof adv_params);
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_start failed; rc=%d", rc);
        return;
    }
    ESP_LOGI(TAG, "advertising as '%s'", s_devName);
}

/* ---------------------------------------------------------------------------
 * Observer: scan for the host's connectionless time broadcast. The host
 * advertises a BLE local name "clk:<epoch>:<tzoff>"; we parse it off each
 * matching advertisement and stash it for blePollBroadcastTime(). Runs on the
 * NimBLE host task; passive scan, never connects.
 * ------------------------------------------------------------------------- */
static int gap_disc_event(struct ble_gap_event *event, void *arg)
{
    if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        ble_scan_start();          /* BLE_HS_FOREVER shouldn't end — restart if it does */
        return 0;
    }
    if (event->type != BLE_GAP_EVENT_DISC) return 0;

    struct ble_hs_adv_fields f;
    if (ble_hs_adv_parse_fields(&f, event->disc.data, event->disc.length_data) != 0) return 0;
    if (!f.name || f.name_len < 5) return 0;

    char nm[32];
    uint8_t n = (f.name_len < sizeof(nm) - 1) ? f.name_len : (uint8_t)(sizeof(nm) - 1);
    memcpy(nm, f.name, n);
    nm[n] = 0;
    if (strncmp(nm, "Claude-", 7) == 0) {       /* another pet: the bot stand judges it */
        if (strcmp(nm, s_devName) != 0) stand_heard(nm, event->disc.rssi, esp_timer_get_time());
        return 0;
    }
    if (strncmp(nm, "clk:", 4) != 0) return 0;

    /* "clk:<epoch>:<tzoff>" -> two longs */
    char *end = NULL;
    long ep = strtol(nm + 4, &end, 10);
    long tz = (end && *end == ':') ? strtol(end + 1, NULL, 10) : 0;
    if (ep > 0) {
        s_bcast_epoch = ep;
        s_bcast_tzoff = tz;
        s_bcast_new   = true;
    }
    return 0;
}

static void ble_scan_start(void)
{
    struct ble_gap_disc_params dp;
    memset(&dp, 0, sizeof dp);
    dp.passive           = 0;       /* ACTIVE — send scan reqs so we also receive the
                                     * SCAN_RSP (macOS often carries the name there) */
    dp.filter_duplicates = 0;       /* keep hearing the (repeating) time beacon */
    dp.itvl              = 512;     /* 320ms (in 0.625ms units) — LOW DUTY: a 30ms    */
    dp.window            = 48;      /* window every 320ms (~9%) leaves the radio free  */
                                    /* for the usage connection to ESTABLISH + hold.   */
                                    /* (continuous scan starved it; same values as the */
                                    /* ESPHome beacon that coexisted fine.)            */
    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &dp, gap_disc_event, NULL);
    if (rc != 0) ESP_LOGW(TAG, "scan start failed; rc=%d — time broadcast off", rc);
    else         ESP_LOGI(TAG, "scanning for time broadcast (clk:<epoch>:<tzoff>)");
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "nimble reset; reason=%d", reason);
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) { ESP_LOGE(TAG, "ensure_addr failed; rc=%d", rc); return; }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) { ESP_LOGE(TAG, "infer_auto failed; rc=%d", rc); return; }
    ble_advertise();
    ble_scan_start();            /* also scan for the connectionless time broadcast */
}

static void ble_host_task(void *param)
{
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run();              /* returns only on nimble_port_stop() */
    nimble_port_freertos_deinit();
}

/* ---------------------------------------------------------------------------
 * bleInit — NVS + NimBLE host/controller + register NUS service + host task.
 * Non-fatal on failure: logs and returns; bleConnected()/blePoll() stay inert.
 * ------------------------------------------------------------------------- */
void bleInit(const char *deviceName)
{
    if (deviceName && deviceName[0]) {
        strlcpy(s_devName, deviceName, sizeof(s_devName));
    }

    /* NVS — NimBLE stores PHY calibration / (optional) bonds here. */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s — BLE disabled",
                 esp_err_to_name(ret));
        return;
    }

    ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s — BLE disabled",
                 esp_err_to_name(ret));
        return;
    }

    /* Host config: open / no bonding (the desktop uses open NUS chars). */
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.sm_bonding = 0;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 0;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;

    /* Register GAP + GATT base services, then our NUS service. */
    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(nus_gatt_svcs);
    if (rc != 0) { ESP_LOGE(TAG, "gatts_count_cfg failed; rc=%d", rc); return; }
    rc = ble_gatts_add_svcs(nus_gatt_svcs);
    if (rc != 0) { ESP_LOGE(TAG, "gatts_add_svcs failed; rc=%d", rc); return; }
    /* STEP 11: the OTA service (ota.c). Its failure leaves NUS working. */
    rc = ble_gatts_count_cfg(ota_gatt_svcs());
    if (rc == 0) rc = ble_gatts_add_svcs(ota_gatt_svcs());
    if (rc != 0) ESP_LOGE(TAG, "OTA service not registered; rc=%d", rc);
    /* Improv Wi-Fi (improv.c): any Improv client can give the pet a network. */
    rc = ble_gatts_count_cfg(improv_gatt_svcs());
    if (rc == 0) rc = ble_gatts_add_svcs(improv_gatt_svcs());
    if (rc != 0) ESP_LOGE(TAG, "Improv service not registered; rc=%d", rc);

    rc = ble_svc_gap_device_name_set(s_devName);
    if (rc != 0) { ESP_LOGW(TAG, "device_name_set failed; rc=%d", rc); }

    nimble_port_freertos_init(ble_host_task);
    ESP_LOGI(TAG, "bleInit done — will advertise as '%s' on host sync", s_devName);
}

bool     bleConnected(void) { return connected; }
uint32_t blePasskey(void)   { return 0; }   /* open chars, no pairing — always 0 */

/* Drain the latest scanned time broadcast (main thread). True iff a new one
 * arrived since the last call. */
bool blePollBroadcastTime(long *epoch, long *tzoff)
{
    if (!s_bcast_new) return false;
    s_bcast_new = false;
    if (epoch) *epoch = s_bcast_epoch;
    if (tzoff) *tzoff = s_bcast_tzoff;
    return true;
}

/* ===========================================================================
 * Tiny flat-JSON scanner — replaces ArduinoStudio's deserializeJson for the
 * heartbeat lines. Same dependency-free strstr/copy approach gif.cpp uses.
 * Heartbeat keys are flat ints/longs/strings plus one nested "prompt" object;
 * find_key locates "key": and we read the value right after.
 * ===========================================================================*/
static const char *find_key(const char *buf, const char *key)
{
    char pat[40];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(buf, pat);
    if (!p) return NULL;
    p = strchr(p + strlen(pat), ':');
    return p ? p + 1 : NULL;
}

/* Read an integer value at/after p; returns def if no digits found. */
static long read_long(const char *p, long def)
{
    if (!p) return def;
    while (*p == ' ' || *p == '\t') p++;
    char *end = NULL;
    long v = strtol(p, &end, 10);
    if (end == p) return def;   /* no number consumed (e.g. null/true) */
    return v;
}
static int read_int(const char *p, int def) { return (int)read_long(p, (long)def); }

/* Copy the next JSON string literal at/after p into out; returns false if the
 * value isn't a quoted string before the next ',' or '}' (so "msg":null -> ""). */
static bool copy_string(const char *p, char *out, size_t n)
{
    if (n) out[0] = 0;
    if (!p) return false;
    while (*p && *p != '"' && *p != ',' && *p != '}') p++;
    if (*p != '"') return false;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < n) {
        /* minimal unescaping: \" \\ \n \t */
        if (*p == '\\' && p[1]) {
            char c = p[1];
            out[i++] = (c == 'n') ? '\n' : (c == 't') ? '\t' : c;
            p += 2;
            continue;
        }
        out[i++] = *p++;
    }
    if (*p && *p != '"' && i > 0) {
        /* cut short: never leave half a UTF-8 character at the end. Back up over continuation
         * bytes and drop the lead byte whose sequence did not fit. */
        size_t j = i;
        while (j > 0 && ((unsigned char)out[j - 1] & 0xC0) == 0x80) j--;
        if (j > 0 && ((unsigned char)out[j - 1] & 0x80)) {
            unsigned char lead = (unsigned char)out[j - 1];
            size_t need = (lead >= 0xF0) ? 4 : (lead >= 0xE0) ? 3 : 2;
            if (i - (j - 1) < need) i = j - 1;
        }
    }
    if (i < n) out[i] = 0;
    return true;
}

/* ---------------------------------------------------------------------------
 * parseLine — port of ble.cpp::parseLine (ArduinoJson -> flat scanner). Parses
 * one JSON line into *out. Heartbeat fields: total/running/waiting/
 * tokens_today|tokens/msg + optional reset5h/reset7d/pct5h/pct7d/
 * brightness + optional prompt{tool,hint}.
 * Returns false if the line isn't a JSON object. Runs on the calling (main)
 * thread, never the NimBLE host task.
 * ------------------------------------------------------------------------- */
static bool parseLine(const char *line, BleSnapshot *out)
{
    /* Cheap object guard (caller already checks line[0]=='{', double-check). */
    const char *brace = strchr(line, '{');
    if (!brace) return false;

    out->total      = read_int (find_key(line, "total"),      0);
    out->running    = read_int (find_key(line, "running"),    0);
    out->waiting    = read_int (find_key(line, "waiting"),    0);
    /* moremas/build-with-claude's Cardputer Buddy heartbeat carries both
     * "tokens" (turn-local) and "tokens_today" (the UI line). Preserve the
     * legacy parser contract by exposing today's total through out->tokens
     * when present; old hosts that only send "tokens" still work. */
    long tokens_today = read_long(find_key(line, "tokens_today"), -1L);
    out->tokens     = (tokens_today >= 0)
                      ? tokens_today
                      : read_long(find_key(line, "tokens"),     0L);
    out->reset5h    = read_long(find_key(line, "reset5h"),   -1L);
    out->reset7d    = read_long(find_key(line, "reset7d"),   -1L);
    out->pct5h      = read_int (find_key(line, "pct5h"),     -1);
    out->pct7d      = read_int (find_key(line, "pct7d"),     -1);
    out->brightness = read_int (find_key(line, "brightness"),-1);
    out->epoch      = read_long(find_key(line, "epoch"),     -1L);   /* host clock, UTC seconds */
    out->tzoff      = read_long(find_key(line, "tzoff"),      0L);   /* local offset, seconds   */

    const char *mp = find_key(line, "msg");
    if (!copy_string(mp, out->msg, sizeof(out->msg))) out->msg[0] = 0;
    if (!copy_string(find_key(line, "who"), out->who, sizeof(out->who))) out->who[0] = 0;

    /* Optional nested prompt{tool,hint}. find_key("prompt") lands on the '{';
     * scoping the tool/hint search to the object is unnecessary here since the
     * heartbeat has no other "tool"/"hint" keys, so we just scan from the prompt
     * value onward (mirrors the Arduino "<tool>: <hint>" formatting). */
    const char *pp = find_key(line, "prompt");
    if (pp) {
        while (*pp == ' ' || *pp == '\t') pp++;
        if (*pp == '{') {
            /* Sized so "<tool>: <hint>" provably fits out->prompt[96]:
             * 31 (tool) + 2 (": ") + 61 (hint) + 1 (NUL) = 95 <= 96. This keeps
             * the compiler's -Werror=format-truncation happy while preserving the
             * Arduino "<tool>: <hint>" formatting. */
            char tool[32] = "", hint[62] = "";
            copy_string(find_key(pp, "tool"), tool, sizeof(tool));
            copy_string(find_key(pp, "hint"), hint, sizeof(hint));
            snprintf(out->prompt, sizeof(out->prompt), "%s: %s", tool, hint);
        } else {
            out->prompt[0] = 0;
        }
    } else {
        out->prompt[0] = 0;
    }

    if (!firstSnapshotLogged) {
        firstSnapshotLogged = true;
        ESP_LOGI(TAG, "snapshot total=%d running=%d waiting=%d tokens=%ld",
                 out->total, out->running, out->waiting, out->tokens);
    }
    return true;
}

/* ---------------------------------------------------------------------------
 * blePoll — ported VERBATIM from ble.cpp. Drain RX ring under lock (copy only),
 * append to the main-thread line buffer, scan for '\n'/'\r'-terminated lines,
 * parse each (keep the newest), preserve the unterminated remainder.
 * ------------------------------------------------------------------------- */
bool blePoll(BleSnapshot *out)
{
    /* Drain whatever the RX callback buffered (under lock, copy only). */
    uint8_t scratch[RX_CAP];
    size_t  n;
    portENTER_CRITICAL(&rxMux);
    n = rxLen;
    if (n > 0) memcpy(scratch, rxBuf, n);
    rxLen = 0;
    portEXIT_CRITICAL(&rxMux);

    /* Append into the line-assembly buffer (main-thread owned, no lock). */
    for (size_t i = 0; i < n; i++) {
        if (lineLen >= sizeof(lineBuf) - 1) lineLen = 0;  /* overflow: drop partial */
        lineBuf[lineLen++] = (char)scratch[i];
    }

    /* Scan for complete '\n'/'\r'-terminated lines. Parse each; keep the last
     * full snapshot. If multiple arrived this call, *out reflects the newest. */
    bool   got   = false;
    size_t start = 0;
    for (size_t i = 0; i < lineLen; i++) {
        char c = lineBuf[i];
        if (c == '\n' || c == '\r') {
            size_t segLen = i - start;
            if (segLen > 0) {
                char tmp[RX_CAP];
                if (segLen > sizeof(tmp) - 1) segLen = sizeof(tmp) - 1;
                memcpy(tmp, &lineBuf[start], segLen);
                tmp[segLen] = 0;
                if (tmp[0] == '{') {
                    if (parseLine(tmp, out)) got = true;
                }
            }
            start = i + 1;
        }
    }

    /* Preserve the unterminated remainder for the next call. */
    if (start > 0) {
        size_t rem = lineLen - start;
        if (rem > 0) memmove(lineBuf, &lineBuf[start], rem);
        lineLen = rem;
    }

    return got;
}
