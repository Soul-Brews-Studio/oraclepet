/*
 * improv.c — Improv Wi-Fi over BLE (https://www.improv-wifi.com/ble/), so any Improv client can
 * give the pet a WiFi network: improv-wifi.com in Chrome, Home Assistant, or OraclePet.
 *
 * Service 00467768-6228-2272-4663-277478268000:
 *   …8001 current state  read/notify  0x02 authorized (no button needed), 0x03 provisioning, 0x04 provisioned
 *   …8002 error state    read/notify  0 none, 1 invalid RPC, 2 unknown RPC, 3 unable to connect, 0xFF unknown
 *   …8003 RPC command    write        cmd, len, data, checksum (sum of the bytes before it, low byte)
 *                                     0x01 wifi settings: ssid len, ssid, password len, password; 0x02 identify
 *   …8004 RPC result     read/notify  cmd, len, strings (len + bytes each), checksum
 *   …8005 capabilities   read         bit 0: identify
 * The pet keeps the voice server URL it had (VOICE.md); a new network goes through wifi_set().
 */
#include "improv.h"

#include <string.h>

#include "audio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "wifi.h"

static const char *TAG = "improv";

/* 00467768-6228-2272-4663-2774782680xx, little-endian as NimBLE wants it */
#define IMPROV_UUID(x) BLE_UUID128_INIT(x, 0x80, 0x26, 0x78, 0x74, 0x27, 0x63, 0x46, 0x72, 0x22, 0x28, 0x62, 0x68, 0x77, 0x46, 0x00)
static const ble_uuid128_t SVC   = IMPROV_UUID(0x00);
static const ble_uuid128_t STATE = IMPROV_UUID(0x01);
static const ble_uuid128_t ERR   = IMPROV_UUID(0x02);
static const ble_uuid128_t RPC   = IMPROV_UUID(0x03);
static const ble_uuid128_t RES   = IMPROV_UUID(0x04);
static const ble_uuid128_t CAPS  = IMPROV_UUID(0x05);

enum { ST_AUTHORIZED = 0x02, ST_PROVISIONING = 0x03, ST_PROVISIONED = 0x04 };
enum { E_NONE = 0, E_INVALID = 1, E_UNKNOWN = 2, E_CONNECT = 3 };

static uint8_t  s_state = ST_AUTHORIZED, s_err = E_NONE;
static uint8_t  s_res[80];
static size_t   s_res_n;
static uint16_t s_state_h, s_err_h, s_res_h, s_conn = BLE_HS_CONN_HANDLE_NONE;
static TimerHandle_t s_timeout;

const ble_uuid128_t *improv_uuid(void) { return &SVC; }
uint8_t improv_state(void) { return s_state; }

static void notify(uint16_t h, const void *d, size_t n)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || !h) return;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(d, n);
    if (om) ble_gatts_notify_custom(s_conn, h, om);
}

static void set_state(uint8_t st) { s_state = st; notify(s_state_h, &s_state, 1); }
static void set_err(uint8_t e)    { s_err = e;    notify(s_err_h, &s_err, 1); }

static void timeout_cb(TimerHandle_t t)          /* no IP within 20 s: "unable to connect" */
{
    (void)t;
    if (s_state == ST_PROVISIONING) { set_err(E_CONNECT); set_state(ST_AUTHORIZED); }
}

void improv_wifi_note(const char *st)            /* from the WiFi state changes */
{
    if (strncmp(st, "up ", 3) != 0) return;
    if (s_state != ST_PROVISIONING) {             /* on WiFi from the saved network: say so */
        if (s_state != ST_PROVISIONED) set_state(ST_PROVISIONED);
        return;
    }
    xTimerStop(s_timeout, 0);
    /* result: the device's URL (Improv shows it as "visit device") */
    char url[40];
    int ul = snprintf(url, sizeof url, "http://%s", st + 3);
    s_res[0] = 0x01; s_res[1] = (uint8_t)(1 + ul); s_res[2] = (uint8_t)ul;
    memcpy(s_res + 3, url, ul);
    uint8_t sum = 0;
    for (int i = 0; i < 3 + ul; i++) sum += s_res[i];
    s_res[3 + ul] = sum;
    s_res_n = 4 + ul;
    set_state(ST_PROVISIONED);
    notify(s_res_h, s_res, s_res_n);
    ESP_LOGI(TAG, "provisioned: %s", st);
}

static int rpc(const uint8_t *d, size_t n)
{
    if (n < 3 || (size_t)d[1] + 3 != n) { set_err(E_INVALID); return 0; }
    uint8_t sum = 0;
    for (size_t i = 0; i + 1 < n; i++) sum += d[i];
    if (sum != d[n - 1]) { set_err(E_INVALID); return 0; }
    const uint8_t *p = d + 2, *end = d + 2 + d[1];
    switch (d[0]) {
    case 0x01: {                                   /* wifi settings */
        char ssid[33], pass[65];
        if (p >= end || p[0] >= sizeof ssid || p + 1 + p[0] > end) { set_err(E_INVALID); return 0; }
        memcpy(ssid, p + 1, p[0]); ssid[p[0]] = 0; p += 1 + p[0];
        if (p >= end || p[0] >= sizeof pass || p + 1 + p[0] > end) { set_err(E_INVALID); return 0; }
        memcpy(pass, p + 1, p[0]); pass[p[0]] = 0;
        set_err(E_NONE);
        set_state(ST_PROVISIONING);
        ESP_LOGI(TAG, "network '%s' from an Improv client", ssid);
        if (!wifi_set(ssid, pass, wifi_url())) { set_err(E_CONNECT); set_state(ST_AUTHORIZED); return 0; }
        xTimerStart(s_timeout, 0);
        return 0;
    }
    case 0x02:                                     /* identify: a short chirp */
        audio_tone(1200, 150);
        return 0;
    default:
        set_err(E_UNKNOWN);
        return 0;
    }
}

static int access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr;
    s_conn = conn;
    uintptr_t which = (uintptr_t)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        static const uint8_t caps = 0x01;          /* identify */
        switch (which) {
        case 1: return os_mbuf_append(ctxt->om, &s_state, 1) ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
        case 2: return os_mbuf_append(ctxt->om, &s_err, 1) ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
        case 4: return os_mbuf_append(ctxt->om, s_res, s_res_n) ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
        case 5: return os_mbuf_append(ctxt->om, &caps, 1) ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
        }
        return 0;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR && which == 3) {
        uint8_t buf[200];
        uint16_t n = 0;
        if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof buf, &n) != 0) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        return rpc(buf, n);
    }
    return 0;
}

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &SVC.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            { .uuid = &STATE.u, .access_cb = access, .arg = (void *)1, .val_handle = &s_state_h,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY },
            { .uuid = &ERR.u, .access_cb = access, .arg = (void *)2, .val_handle = &s_err_h,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY },
            { .uuid = &RPC.u, .access_cb = access, .arg = (void *)3,
              .flags = BLE_GATT_CHR_F_WRITE },
            { .uuid = &RES.u, .access_cb = access, .arg = (void *)4, .val_handle = &s_res_h,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY },
            { .uuid = &CAPS.u, .access_cb = access, .arg = (void *)5,
              .flags = BLE_GATT_CHR_F_READ },
            { 0 },
        },
    },
    { 0 },
};

const struct ble_gatt_svc_def *improv_gatt_svcs(void)
{
    if (!s_timeout) s_timeout = xTimerCreate("improv", pdMS_TO_TICKS(20000), pdFALSE, NULL, timeout_cb);
    if (wifi_is_up()) s_state = ST_PROVISIONED;
    return s_svcs;
}

void improv_on_disconnect(void) { s_conn = BLE_HS_CONN_HANDLE_NONE; }
