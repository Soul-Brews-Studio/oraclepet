/*
 * OraclePet OTA base: the smallest firmware that takes any ESP32-S3 app image over BLE.
 *
 * Flash it once over USB (the web flasher's "Base" button). It lives in the factory
 * partition, advertises as Claude-XXXX with the same Nordic UART + Pet OTA services as the
 * pet, and installs whatever app image the host sends (BEGIN / data / END, PROTOCOL.md 4.4)
 * into ota_0 or ota_1, then boots it. OTA never writes the factory partition, so the base
 * stays on the flash: erase otadata (the web flasher's "Back to base" button) and the board
 * boots the base again.
 *
 * Checks before booting a new image: app image magic, built for the ESP32-S3, fits the slot,
 * CRC-32 of the whole transfer, and esp_ota_end()'s own image + SHA-256 check. There is no
 * pairing: anyone in range can send an image while the base advertises.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "base";

#define SB_SIZE   (32 * 1024)   /* stream buffer between the BLE host task and the writer */
#define WINDOW    (16 * 1024)   /* bytes the host may send beyond the last ack            */
#define ACK_EVERY (4 * 1024)
#define CHUNK     4096
#define IDLE_MS   15000

/* 6e4000xx-b5a3-f393-e0a9-e50e24dcca9e, little-endian as NimBLE wants it */
#define PET_UUID(b) BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, \
                                     0x93, 0xf3, 0xa3, 0xb5, (b), 0x00, 0x40, 0x6e)
static const ble_uuid128_t NUS_SVC  = PET_UUID(0x01);
static const ble_uuid128_t NUS_RX   = PET_UUID(0x02);
static const ble_uuid128_t NUS_TX   = PET_UUID(0x03);
static const ble_uuid128_t OTA_SVC  = PET_UUID(0x10);
static const ble_uuid128_t OTA_CTRL = PET_UUID(0x11);
static const ble_uuid128_t OTA_DATA = PET_UUID(0x12);

enum { ST_IDLE, ST_STARTING, ST_RECEIVING, ST_FINISHING };
static volatile int      s_state = ST_IDLE;
static volatile bool     s_abort, s_end;
static volatile uint32_t s_size, s_crc_expect, s_received;
static StreamBufferHandle_t s_sb;
static TaskHandle_t      s_task;
static uint16_t          s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t          s_ctrl_handle, s_tx_handle;
static bool              s_subscribed;
static uint8_t           s_addr_type;
static char              s_name[16] = "Claude-0000";

/* ---- CRC-32 (zlib), the one the host computes ---- */
static uint32_t s_crc_table[256];
static void crc_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        s_crc_table[i] = c;
    }
}
static uint32_t crc_update(uint32_t crc, const uint8_t *p, size_t n)
{
    crc = ~crc;
    while (n--) crc = s_crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static void notify(const char *fmt, ...)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || !s_subscribed) return;
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n <= 0 || n >= (int)sizeof line) return;
    for (int tries = 0; tries < 5; tries++) {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(line, (uint16_t)n);
        if (om && ble_gatts_notify_custom(s_conn, s_ctrl_handle, om) == 0) return;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGW(TAG, "notify dropped: %s", line);
}

static void fail(esp_ota_handle_t *h, const char *why)
{
    ESP_LOGE(TAG, "update failed: %s", why);
    if (*h) { esp_ota_abort(*h); *h = 0; }
    s_state = ST_IDLE;
    notify("{\"ota\":\"error\",\"why\":\"%s\"}", why);
}

/* Image header (24) + first segment header (8), then esp_app_desc_t. */
#define DESC_OFF (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t))
static bool check_image(const uint8_t *hdr, char *why, size_t whylen)
{
    const esp_image_header_t *ih = (const esp_image_header_t *)hdr;
    const esp_app_desc_t *d = (const esp_app_desc_t *)(hdr + DESC_OFF);
    if (ih->magic != ESP_IMAGE_HEADER_MAGIC || d->magic_word != ESP_APP_DESC_MAGIC_WORD) {
        snprintf(why, whylen, "not an ESP app image");
        return false;
    }
    if (ih->chip_id != ESP_CHIP_ID_ESP32S3) {
        snprintf(why, whylen, "not built for the ESP32-S3 (chip id %u)", (unsigned)ih->chip_id);
        return false;
    }
    ESP_LOGI(TAG, "incoming %.32s %.32s, built %.16s %.16s", d->project_name, d->version, d->date, d->time);
    return true;
}

static void ota_task(void *arg)
{
    static uint8_t buf[CHUNK];
    static uint8_t hdr[DESC_OFF + sizeof(esp_app_desc_t)];
    char why[64];

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);       /* woken by BEGIN */
        if (s_state != ST_STARTING) continue;

        esp_ota_handle_t h = 0;
        const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
        if (!part) { fail(&h, "no OTA slot in the partition table"); continue; }
        if (s_size > part->size) { fail(&h, "image larger than the slot"); continue; }
        esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &h);
        if (err != ESP_OK) { fail(&h, esp_err_to_name(err)); continue; }

        ESP_LOGI(TAG, "receiving %u bytes into %s", (unsigned)s_size, part->label);
        xStreamBufferReset(s_sb);
        uint32_t crc = 0, written = 0, last_ack = 0, idle_ms = 0;
        size_t hdr_len = 0;
        bool ok = true;
        s_state = ST_RECEIVING;
        notify("{\"ota\":\"ready\",\"size\":%u,\"window\":%u}", (unsigned)s_size, WINDOW);

        while (written < s_size) {
            if (s_abort) { fail(&h, s_received > s_size ? "too many bytes" : "aborted"); ok = false; break; }
            size_t n = xStreamBufferReceive(s_sb, buf, sizeof buf, pdMS_TO_TICKS(500));
            if (n == 0) {
                idle_ms += 500;
                if (idle_ms >= IDLE_MS) { fail(&h, "timed out waiting for data"); ok = false; break; }
                notify("{\"ota\":\"ack\",\"off\":%u}", (unsigned)written);
                continue;
            }
            idle_ms = 0;
            if (hdr_len < sizeof hdr) {
                size_t take = sizeof hdr - hdr_len < n ? sizeof hdr - hdr_len : n;
                memcpy(hdr + hdr_len, buf, take);
                hdr_len += take;
                if (hdr_len == sizeof hdr && !check_image(hdr, why, sizeof why)) { fail(&h, why); ok = false; break; }
            }
            err = esp_ota_write(h, buf, n);
            if (err != ESP_OK) { fail(&h, esp_err_to_name(err)); ok = false; break; }
            crc = crc_update(crc, buf, n);
            written += n;
            if (written - last_ack >= ACK_EVERY || written == s_size) {
                last_ack = written;
                notify("{\"ota\":\"ack\",\"off\":%u}", (unsigned)written);
            }
        }
        if (!ok) continue;

        s_state = ST_FINISHING;
        for (int i = 0; i < 100 && !s_end && !s_abort; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
            if (i % 5 == 4 && !s_end) notify("{\"ota\":\"ack\",\"off\":%u}", (unsigned)written);
        }
        if (!s_end) { fail(&h, s_abort ? "aborted" : "no END from the host"); continue; }
        if (crc != s_crc_expect) {
            snprintf(why, sizeof why, "crc mismatch %08lx != %08lx", (unsigned long)crc, (unsigned long)s_crc_expect);
            fail(&h, why);
            continue;
        }
        err = esp_ota_end(h);                          /* validates the image and its SHA-256 */
        h = 0;
        if (err != ESP_OK) { s_state = ST_IDLE; notify("{\"ota\":\"error\",\"why\":\"%s\"}", esp_err_to_name(err)); continue; }
        err = esp_ota_set_boot_partition(part);
        if (err != ESP_OK) { s_state = ST_IDLE; notify("{\"ota\":\"error\",\"why\":\"%s\"}", esp_err_to_name(err)); continue; }

        ESP_LOGI(TAG, "image ok; booting %s", part->label);
        notify("{\"ota\":\"done\"}");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }
}

/* ---- GATT ---- */
static int ctrl_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        const esp_app_desc_t *me = esp_app_get_description();
        const esp_partition_t *run = esp_ota_get_running_partition();
        char sha[17];
        esp_app_get_elf_sha256(sha, sizeof sha);
        char j[256];
        int n = snprintf(j, sizeof j,
                         "{\"ver\":\"%s\",\"project\":\"%s\",\"built\":\"%s %s\",\"slot\":\"%s\",\"sha\":\"%s\","
                         "\"state\":\"factory\",\"base\":true}",
                         me->version, me->project_name, me->date, me->time, run ? run->label : "?", sha);
        if (n >= (int)sizeof j) n = sizeof j - 1;
        return os_mbuf_append(ctxt->om, j, n) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;

    uint8_t cmd[16];
    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, cmd, sizeof cmd, &len) != 0 || len == 0) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    switch (cmd[0]) {
    case 0x01: {                                       /* BEGIN u32 size, u32 crc32 */
        if (len < 9) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        if (s_state != ST_IDLE) { notify("{\"ota\":\"error\",\"why\":\"busy\"}"); return 0; }
        memcpy((void *)&s_size, cmd + 1, 4);
        memcpy((void *)&s_crc_expect, cmd + 5, 4);
        s_received = 0;
        s_abort = s_end = false;
        s_state = ST_STARTING;
        xTaskNotifyGive(s_task);
        return 0;
    }
    case 0x02: s_end = true; return 0;                 /* END */
    case 0x03: s_abort = true; return 0;               /* ABORT */
    case 0x07: return 0;                               /* PROGRESS: nothing to draw */
    default:
        notify("{\"ota\":\"error\",\"why\":\"the base only installs firmware (command 0x%02x)\"}", cmd[0]);
        return 0;
    }
}

static int data_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR || s_state != ST_RECEIVING) return 0;
    uint8_t tmp[520];
    uint16_t n = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, tmp, sizeof tmp, &n) != 0 || n == 0) return 0;
    s_received += n;
    if (s_received > s_size) { s_abort = true; return 0; }
    if (xStreamBufferSend(s_sb, tmp, n, 0) != n) s_abort = true;   /* host ignored the window */
    return 0;
}

static int nus_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    return 0;                                          /* heartbeats are accepted and ignored */
}

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &NUS_SVC.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = &NUS_RX.u, .access_cb = nus_access, .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP },
            { .uuid = &NUS_TX.u, .access_cb = nus_access, .val_handle = &s_tx_handle,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY },
            { 0 }
        },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &OTA_SVC.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = &OTA_CTRL.u, .access_cb = ctrl_access, .val_handle = &s_ctrl_handle,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY },
            { .uuid = &OTA_DATA.u, .access_cb = data_access, .flags = BLE_GATT_CHR_F_WRITE_NO_RSP },
            { 0 }
        },
    },
    { 0 }
};

/* ---- GAP ---- */
static void advertise(void);

static int gap_event(struct ble_gap_event *e, void *arg)
{
    switch (e->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (e->connect.status == 0) s_conn = e->connect.conn_handle;
        else advertise();
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_subscribed = false;
        if (s_state != ST_IDLE) s_abort = true;      /* a transfer in flight is abandoned */
        advertise();
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (e->subscribe.attr_handle == s_ctrl_handle) s_subscribed = e->subscribe.cur_notify;
        return 0;
    default:
        return 0;
    }
}

static void advertise(void)
{
    struct ble_hs_adv_fields f = {0}, rsp = {0};
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.name = (uint8_t *)s_name;
    f.name_len = strlen(s_name);
    f.name_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&f);
    if (rc != 0) { ESP_LOGE(TAG, "adv fields rc=%d", rc); return; }
    rsp.uuids128 = (ble_uuid128_t *)&NUS_SVC;
    rsp.num_uuids128 = 1;
    rsp.uuids128_is_complete = 0;
    ble_gap_adv_rsp_set_fields(&rsp);

    struct ble_gap_adv_params p = {0};
    p.conn_mode = BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) ESP_LOGE(TAG, "adv start rc=%d", rc);
    else ESP_LOGI(TAG, "advertising as %s", s_name);
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_addr_type);
    advertise();
}

static void host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void app_main(void)
{
    const esp_app_desc_t *me = esp_app_get_description();
    ESP_LOGI(TAG, "%s %s: waiting for firmware over BLE", me->project_name, me->version);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* Claude-XXXX from the BT MAC, so the BLE tool's name filter finds it. */
    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_BT) == ESP_OK) snprintf(s_name, sizeof s_name, "Claude-%02X%02X", mac[4], mac[5]);

    crc_init();
    s_sb = xStreamBufferCreate(SB_SIZE, 1);
    if (!s_sb || xTaskCreate(ota_task, "ota", 6144, NULL, 5, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "out of memory");
        return;
    }

    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.sm_bonding = 0;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 0;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ESP_ERROR_CHECK(ble_gatts_count_cfg(s_svcs));
    ESP_ERROR_CHECK(ble_gatts_add_svcs(s_svcs));
    ble_svc_gap_device_name_set(s_name);
    nimble_port_freertos_init(host_task);
}
