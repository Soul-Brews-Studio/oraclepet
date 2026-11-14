/*
 * ota.c — jc3248-pet-idf STEP 11: firmware update over BLE. Protocol in ota.h.
 *
 * Two tasks touch an update:
 *   - the NimBLE host task runs the GATT callbacks. It only parses commands and
 *     copies data bytes into a stream buffer; it never touches flash, because a
 *     sector erase takes tens of ms and would stall the link.
 *   - ota_task (created on the first BEGIN) owns the esp_ota handle: it drains
 *     the stream buffer into the idle slot, keeps a CRC-32, and acks progress.
 *
 * Flow control: write-without-response has no backpressure, so the host keeps
 * at most WINDOW bytes beyond the last ack in flight. The stream buffer is
 * bigger than the window, so a well-behaved host can never overflow it.
 */
#include "ota.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"

#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>
#include "esp_littlefs.h"
#include "fs.h"
#include "gif.h"
#include "petapp.h"
#include "audio.h"
#include "voice.h"
#include "wifi.h"

#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_att.h"

static const char *TAG = "ota";

#define SB_SIZE   (32 * 1024)   /* stream buffer between the host task and ota_task */
#define WINDOW    (16 * 1024)   /* bytes the host may send beyond the last ack       */
#define ACK_EVERY (4 * 1024)
#define CHUNK     4096
#define IDLE_MS   15000         /* no data for this long: give up                    */

/* 6e400010/11/12-b5a3-f393-e0a9-e50e24dcca9e (NUS family, LSB first) */
#define PET_UUID(b) BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, \
                                     0x93, 0xf3, 0xa3, 0xb5, (b), 0x00, 0x40, 0x6e)
static const ble_uuid128_t OTA_SVC_UUID  = PET_UUID(0x10);
static const ble_uuid128_t OTA_CTRL_UUID = PET_UUID(0x11);
static const ble_uuid128_t OTA_DATA_UUID = PET_UUID(0x12);
static const ble_uuid128_t OTA_MIC_UUID  = PET_UUID(0x13);   /* notify: the mic's audio (LISTEN) */

enum { ST_IDLE, ST_STARTING, ST_RECEIVING, ST_FINISHING };

static volatile int      s_state = ST_IDLE;
/* What the transfer writes: the idle app slot (full OTA) or one file under
 * /littlefs (partial OTA: an animation pack, file by file). */
enum { KIND_APP, KIND_FILE, KIND_STREAM };
static volatile int      s_kind = KIND_APP;
static char              s_path[96];          /* KIND_FILE: "characters/<pack>/<file>" */
static char              s_pack_req[24];      /* SELECT asked for this pack; main thread applies it */
static volatile bool     s_pack_pending;
static char              s_app_req[24];       /* RUN_APP: "" = stop */
static volatile bool     s_app_pending;
static volatile int      s_host_pct = -1;     /* PROGRESS from the host: the batch's overall % */
static volatile int64_t  s_host_pct_us;
static volatile bool     s_abort, s_end;
static volatile uint32_t s_size, s_crc_expect, s_received, s_written;
static StreamBufferHandle_t s_sb;
static TaskHandle_t      s_task;
static uint16_t          s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t          s_ctrl_handle;
static bool              s_subscribed;
static uint16_t          s_mic_handle;
static bool              s_mic_subscribed;

/* ---- CRC-32 (zlib polynomial), the same one the host computes ---- */
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

/* ---- notifications on the control char ---- */
static void notify(const char *fmt, ...)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || !s_subscribed) return;
    char line[200];                                    /* the LISTEN "heard" line is ~110 bytes */
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n <= 0 || n >= (int)sizeof(line)) {
        ESP_LOGE(TAG, "status line too long (%d bytes), not sent", n);   /* never drop silently */
        return;
    }
    for (int tries = 0; tries < 5; tries++) {        /* mbufs can run out mid-transfer */
        struct os_mbuf *om = ble_hs_mbuf_from_flat(line, (uint16_t)n);
        if (om && ble_gatts_notify_custom(s_conn, s_ctrl_handle, om) == 0) return;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGW(TAG, "notify dropped: %s", line);
}

/* WIFI_SCAN results: {"ota":"net","ssid":"…","rssi":-52,"ch":6,"open":false} (quotes and
 * backslashes in a name are escaped; a name longer than the line is cut) */
static void scan_found(const char *ssid, int rssi, int ch, bool open)
{
    char esc[70];
    size_t k = 0;
    for (const char *p = ssid; *p && k + 2 < sizeof esc; p++) {
        if (*p == '"' || *p == '\\') esc[k++] = '\\';
        esc[k++] = (unsigned char)*p < 0x20 ? '?' : *p;
    }
    esc[k] = 0;
    notify("{\"ota\":\"net\",\"ssid\":\"%s\",\"rssi\":%d,\"ch\":%d,\"open\":%s}", esc, rssi, ch, open ? "true" : "false");
}

/* WiFi state for the host: {"ota":"wifi","state":"up","ip":"192.168.1.23"} or "connecting" */
void ota_wifi_note(const char *st)
{
    if (strncmp(st, "up ", 3) == 0) notify("{\"ota\":\"wifi\",\"state\":\"up\",\"ip\":\"%.20s\"}", st + 3);
    else notify("{\"ota\":\"wifi\",\"state\":\"%.40s\"}", st);
}

static void fail(esp_ota_handle_t *h, const char *why)
{
    ESP_LOGE(TAG, "update failed: %s", why);
    if (*h) { esp_ota_abort(*h); *h = 0; }
    s_state = ST_IDLE;
    notify("{\"ota\":\"error\",\"why\":\"%s\"}", why);
}

/* The app description sits right after the image header and the first segment
 * header. Refuse an image that is not this project, before writing any more. */
#define DESC_OFF (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t))
static bool check_desc(const uint8_t *hdr, char *why, size_t whylen)
{
    const esp_app_desc_t *d = (const esp_app_desc_t *)(hdr + DESC_OFF);
    if (hdr[0] != ESP_IMAGE_HEADER_MAGIC || d->magic_word != ESP_APP_DESC_MAGIC_WORD) {
        snprintf(why, whylen, "not an ESP app image");
        return false;
    }
    const esp_app_desc_t *me = esp_app_get_description();
    if (strncmp(d->project_name, me->project_name, sizeof(d->project_name)) != 0) {
        snprintf(why, whylen, "wrong firmware (%.24s)", d->project_name);
        return false;
    }
    ESP_LOGI(TAG, "incoming %s %s, built %s %s", d->project_name, d->version, d->date, d->time);
    return true;
}

/* "characters/<pack>/<file>" or "apps/<name>.wasm": lower-case letters, digits,
 * '-', '_', '.', no "..". */
static bool safe_path(const char *p)
{
    if (strstr(p, "..")) return false;
    if (strncmp(p, "apps/", 5) == 0 || strncmp(p, "sounds/", 7) == 0) {   /* one level, fixed extension */
        size_t n = strlen(p);
        const char *ext = p[0] == 'a' ? ".wasm" : ".ima";
        size_t pre = p[0] == 'a' ? 5 : 7, el = strlen(ext);
        if (n < pre + 1 + el || n - pre - el > 23 || strcmp(p + n - el, ext) != 0) return false;
        for (const char *c = p + pre; *c; c++)
            if (*c == '/') return false;
        for (const char *c = p + pre; *c; c++)
            if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-' || *c == '_' || *c == '.')) return false;
        return true;
    }
    if (strncmp(p, "characters/", 11) != 0) return false;
    int slashes = 0;
    for (const char *c = p; *c; c++) {
        if (*c == '/') { slashes++; continue; }
        if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-' || *c == '_' || *c == '.')) return false;
    }
    return slashes == 2;
}

static void file_fail(FILE **f, const char *tmp, const char *why)
{
    if (*f) { fclose(*f); *f = NULL; }
    unlink(tmp);
    s_state = ST_IDLE;
    ESP_LOGE(TAG, "file %s failed: %s", s_path, why);
    notify("{\"ota\":\"error\",\"why\":\"%s\"}", why);
}

/* Partial OTA: stream one file into LittleFS as <path>.part, check its CRC, then
 * rename it over <path>. The pet keeps running; nothing reboots. */
static void file_transfer(void)
{
    static uint8_t buf[CHUNK];
    char full[128], tmp[136], dir[128];
    snprintf(full, sizeof full, "%s/%s", FS_MOUNT_POINT, s_path);
    snprintf(tmp, sizeof tmp, "%s.part", full);
    snprintf(dir, sizeof dir, "%s", full);
    *strrchr(dir, '/') = 0;
    mkdir(dir, 0775);                                  /* the pack's folder; EEXIST is fine */

    size_t total = 0, used = 0;
    if (esp_littlefs_info(FS_PARTITION_LABEL, &total, &used) == ESP_OK && s_size + 8192 > total - used) {
        FILE *none = NULL;
        file_fail(&none, tmp, "storage full");
        return;
    }
    FILE *f = fopen(tmp, "wb");
    if (!f) { file_fail(&f, tmp, "cannot create file"); return; }

    uint32_t crc = 0, last_ack = 0, idle_ms = 0;
    xStreamBufferReset(s_sb);
    s_written = 0;
    s_state = ST_RECEIVING;
    notify("{\"ota\":\"ready\",\"size\":%u,\"window\":%u}", (unsigned)s_size, WINDOW);
    while (s_written < s_size) {
        if (s_abort) { file_fail(&f, tmp, "aborted"); return; }
        size_t n = xStreamBufferReceive(s_sb, buf, sizeof buf, pdMS_TO_TICKS(500));
        if (n == 0) {
            idle_ms += 500;
            if (idle_ms >= IDLE_MS) { file_fail(&f, tmp, "timed out waiting for data"); return; }
            notify("{\"ota\":\"ack\",\"off\":%u}", (unsigned)s_written);
            continue;
        }
        idle_ms = 0;
        if (fwrite(buf, 1, n, f) != n) { file_fail(&f, tmp, "write failed"); return; }
        crc = crc_update(crc, buf, n);
        s_written += n;
        if (s_written - last_ack >= ACK_EVERY || s_written == s_size) {
            last_ack = s_written;
            notify("{\"ota\":\"ack\",\"off\":%u}", (unsigned)s_written);
        }
    }
    s_state = ST_FINISHING;
    for (int i = 0; i < 100 && !s_end && !s_abort; i++) {   /* 10 s for END */
        vTaskDelay(pdMS_TO_TICKS(100));
        if (i % 5 == 4 && !s_end)   /* the last ack may have been lost: repeat it, or the host never sends END */
            notify("{\"ota\":\"ack\",\"off\":%u}", (unsigned)s_written);
    }
    if (!s_end) { file_fail(&f, tmp, "no END from the host"); return; }
    fclose(f);
    f = NULL;
    if (crc != s_crc_expect) { file_fail(&f, tmp, "crc mismatch"); return; }
    unlink(full);
    if (rename(tmp, full) != 0) { file_fail(&f, tmp, "rename failed"); return; }
    ESP_LOGI(TAG, "saved %s (%u bytes)", full, (unsigned)s_size);
    s_state = ST_IDLE;
    notify("{\"ota\":\"saved\",\"path\":\"%s\"}", s_path);
}

/* Live audio (0x0B BEGIN_STREAM): the bytes go to the audio task as they arrive instead
 * of into a file. audio_stream_write() blocks while the ~1 s audio buffer is full; that
 * holds back our acks, so the sender slows to the playback rate. Nothing touches flash. */
static void stream_transfer(void)
{
    static uint8_t buf[CHUNK];
    uint32_t crc = 0, last_ack = 0, idle_ms = 0;
    if (!audio_stream_begin()) { s_state = ST_IDLE; notify("{\"ota\":\"error\",\"why\":\"audio busy\"}"); return; }
    xStreamBufferReset(s_sb);
    s_written = 0;
    s_state = ST_RECEIVING;
    notify("{\"ota\":\"ready\",\"size\":%u,\"window\":%u}", (unsigned)s_size, WINDOW);
    while (s_written < s_size) {
        if (s_abort) break;
        size_t n = xStreamBufferReceive(s_sb, buf, sizeof buf, pdMS_TO_TICKS(500));
        if (n == 0) {
            idle_ms += 500;
            if (idle_ms >= IDLE_MS) break;
            notify("{\"ota\":\"ack\",\"off\":%u}", (unsigned)s_written);
            continue;
        }
        idle_ms = 0;
        for (size_t off = 0; off < n && !s_abort; ) {      /* backpressure: wait for the player */
            off += audio_stream_write(buf + off, n - off, 200);
        }
        crc = crc_update(crc, buf, n);
        s_written += n;
        if (s_written - last_ack >= ACK_EVERY || s_written == s_size) {
            last_ack = s_written;
            notify("{\"ota\":\"ack\",\"off\":%u}", (unsigned)s_written);
        }
    }
    audio_stream_end();
    for (int i = 0; i < 50 && !s_end && !s_abort; i++) {    /* 5 s for END */
        vTaskDelay(pdMS_TO_TICKS(100));
        if (i % 5 == 4 && !s_end)   /* the last ack may have been lost: repeat it */
            notify("{\"ota\":\"ack\",\"off\":%u}", (unsigned)s_written);
    }
    s_state = ST_IDLE;
    if (s_abort || s_written < s_size) { notify("{\"ota\":\"error\",\"why\":\"stream cut short\"}"); return; }
    notify("{\"ota\":\"streamed\",\"bytes\":%u,\"crc\":%s}", (unsigned)s_written, crc == s_crc_expect ? "true" : "false");
}

static void ota_task(void *arg)
{
    static uint8_t buf[CHUNK];
    static uint8_t hdr[DESC_OFF + sizeof(esp_app_desc_t)];
    char why[48];

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);       /* woken by BEGIN */
        if (s_state != ST_STARTING) continue;

        if (s_kind == KIND_FILE) { file_transfer(); continue; }
        if (s_kind == KIND_STREAM) { stream_transfer(); continue; }

        esp_ota_handle_t h = 0;
        const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
        if (!part) { fail(&h, "no OTA slot (USB-flash the OTA partition table)"); continue; }
        if (s_size > part->size) { fail(&h, "image larger than the slot"); continue; }
        esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &h);
        if (err != ESP_OK) { fail(&h, esp_err_to_name(err)); continue; }

        ESP_LOGI(TAG, "receiving %u bytes into %s @0x%lx", (unsigned)s_size, part->label,
                 (unsigned long)part->address);
        xStreamBufferReset(s_sb);
        uint32_t crc = 0, last_ack = 0, idle_ms = 0;
        size_t hdr_len = 0;
        bool ok = true;
        s_written = 0;
        s_state = ST_RECEIVING;
        notify("{\"ota\":\"ready\",\"size\":%u,\"window\":%u}", (unsigned)s_size, WINDOW);
        int64_t t0 = esp_timer_get_time();

        while (s_written < s_size) {
            if (s_abort) { fail(&h, s_received > s_size ? "too many bytes" : "aborted"); ok = false; break; }
            size_t n = xStreamBufferReceive(s_sb, buf, sizeof buf, pdMS_TO_TICKS(500));
            if (n == 0) {
                idle_ms += 500;
                if (idle_ms >= IDLE_MS) { fail(&h, "timed out waiting for data"); ok = false; break; }
                notify("{\"ota\":\"ack\",\"off\":%u}", (unsigned)s_written);   /* re-ack: a lost ack must not stall the host */
                continue;
            }
            idle_ms = 0;
            if (hdr_len < sizeof hdr) {
                size_t take = sizeof hdr - hdr_len < n ? sizeof hdr - hdr_len : n;
                memcpy(hdr + hdr_len, buf, take);
                hdr_len += take;
                if (hdr_len == sizeof hdr && !check_desc(hdr, why, sizeof why)) { fail(&h, why); ok = false; break; }
            }
            err = esp_ota_write(h, buf, n);
            if (err != ESP_OK) { fail(&h, esp_err_to_name(err)); ok = false; break; }
            crc = crc_update(crc, buf, n);
            s_written += n;
            if (s_written - last_ack >= ACK_EVERY || s_written == s_size) {
                last_ack = s_written;
                notify("{\"ota\":\"ack\",\"off\":%u}", (unsigned)s_written);
            }
        }
        if (!ok) continue;

        s_state = ST_FINISHING;
        for (int i = 0; i < 100 && !s_end && !s_abort; i++) {   /* 10 s for END */
            vTaskDelay(pdMS_TO_TICKS(100));
            if (i % 5 == 4 && !s_end)   /* the last ack may have been lost: repeat it, or the host never sends END */
                notify("{\"ota\":\"ack\",\"off\":%u}", (unsigned)s_written);
        }
        if (!s_end) { fail(&h, s_abort ? "aborted" : "no END from the host"); continue; }
        if (crc != s_crc_expect) {
            snprintf(why, sizeof why, "crc mismatch %08lx != %08lx", (unsigned long)crc, (unsigned long)s_crc_expect);
            fail(&h, why);
            continue;
        }
        err = esp_ota_end(h);                          /* checks the image + its SHA-256 */
        h = 0;
        if (err != ESP_OK) { s_state = ST_IDLE; notify("{\"ota\":\"error\",\"why\":\"%s\"}", esp_err_to_name(err)); continue; }
        err = esp_ota_set_boot_partition(part);
        if (err != ESP_OK) { s_state = ST_IDLE; notify("{\"ota\":\"error\",\"why\":\"%s\"}", esp_err_to_name(err)); continue; }

        int64_t ms = (esp_timer_get_time() - t0) / 1000;
        ESP_LOGI(TAG, "image ok: %u bytes in %lld ms (%.1f KB/s); booting %s",
                 (unsigned)s_size, ms, ms ? s_size / 1.024 / ms : 0.0, part->label);
        notify("{\"ota\":\"done\"}");
        vTaskDelay(pdMS_TO_TICKS(1000));               /* let the notify leave */
        esp_restart();
    }
}

static void selftest_done(bool has_mic, bool heard, float db, int rq, int rt);
static void listen_done(const audio_listen_result_t *r);
static bool mic_audio(const uint8_t *d, size_t n);

/* The transfer task and its buffer, made once. At boot, from ota_gatt_svcs(), before WiFi
 * takes its share of internal RAM: made lazily at the first BEGIN, with WiFi up, the 6 KB task
 * stack (which must be internal: the task writes flash) was no longer there. */
static bool ota_prepare(void)
{
    if (s_sb && s_task) return true;
    crc_init();
    if (!s_sb) s_sb = xStreamBufferCreateWithCaps(SB_SIZE, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_sb && !s_task && xTaskCreate(ota_task, "ota", 6144, NULL, 5, &s_task) != pdPASS) s_task = NULL;
    if (!s_sb || !s_task) ESP_LOGE(TAG, "transfer task: out of memory");
    return s_sb && s_task;
}

/* ---- GATT callbacks (NimBLE host task) ---- */
static int ctrl_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        const esp_app_desc_t *me = esp_app_get_description();
        const esp_partition_t *run = esp_ota_get_running_partition();
        /* "sha": the first 16 hex of the ELF SHA-256. Version and build time can match
         * across builds (IDF stamps them only when esp_app_desc.c recompiles); this cannot. */
        char sha[17];
        esp_app_get_elf_sha256(sha, sizeof sha);
        /* "state": valid, or pending until main.c marks this image good (rollback). */
        esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
        if (run) esp_ota_get_state_partition(run, &st);
        const char *state = st == ESP_OTA_IMG_PENDING_VERIFY ? "pending"
                          : st == ESP_OTA_IMG_VALID ? "valid" : st == ESP_OTA_IMG_UNDEFINED ? "factory" : "other";
        /* the packs on the pet, which one shows, and free storage (for partial OTA) */
        char packs[220] = "";
        DIR *d = opendir(FS_MOUNT_POINT "/characters");
        if (d) {
            struct dirent *e;
            size_t k = 0;
            while ((e = readdir(d)) != NULL && k < sizeof packs - 30) {
                if (e->d_name[0] == '.') continue;
                k += snprintf(packs + k, sizeof packs - k, "%s\"%.23s\"", k ? "," : "", e->d_name);
            }
            closedir(d);
        }
        char apps[120] = "";
        d = opendir(FS_MOUNT_POINT "/apps");
        if (d) {
            struct dirent *e;
            size_t k = 0;
            while ((e = readdir(d)) != NULL && k < sizeof apps - 30) {
                size_t l = strlen(e->d_name);
                if (l < 6 || strcmp(e->d_name + l - 5, ".wasm") != 0) continue;
                k += snprintf(apps + k, sizeof apps - k, "%s\"%.*s\"", k ? "," : "", (int)(l - 5 > 23 ? 23 : l - 5), e->d_name);
            }
            closedir(d);
        }
        size_t total = 0, used = 0;
        esp_littlefs_info(FS_PARTITION_LABEL, &total, &used);
        char j[512];
        int n = snprintf(j, sizeof j, "{\"ver\":\"%s\",\"project\":\"%s\",\"built\":\"%s %s\",\"slot\":\"%s\",\"sha\":\"%s\",\"state\":\"%s\","
                         "\"pack\":\"%s\",\"packs\":[%s],\"free\":%u,\"app\":\"%s\",\"apps\":[%s],\"apperr\":\"%.60s\","
                         "\"wifi\":\"%.40s\",\"voice\":%d,\"voicestate\":\"%.30s\"}",
                         me->version, me->project_name, me->date, me->time, run ? run->label : "?", sha, state,
                         gif_pack_name(), packs, (unsigned)((total - used) / 1024),
                         petapp_name(), apps, petapp_error(), wifi_state(), voice_mode(), voice_state());
        if (n >= (int)sizeof j) n = sizeof j - 1;
        return os_mbuf_append(ctxt->om, j, n) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;

    uint8_t cmd[112];
    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, cmd, sizeof cmd, &len) != 0 || len == 0) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

    switch (cmd[0]) {
    case 0x01:                                         /* BEGIN size crc (full OTA: app image) */
    case 0x04:                                         /* BEGIN_FILE size crc len path (partial OTA) */
    case 0x0B: {                                       /* BEGIN_STREAM size crc: a PSND clip played as it arrives */
        if (len < 9) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        if (s_state != ST_IDLE) { notify("{\"ota\":\"error\",\"why\":\"busy\"}"); return 0; }
        if (!ota_prepare()) {
            notify("{\"ota\":\"error\",\"why\":\"out of memory\"}");
            return 0;
        }
        memcpy((void *)&s_size, cmd + 1, 4);
        memcpy((void *)&s_crc_expect, cmd + 5, 4);
        s_kind = cmd[0] == 0x0B ? KIND_STREAM : KIND_APP;
        if (cmd[0] == 0x04) {                          /* BEGIN_FILE: + u8 len + path */
            uint8_t pl = len > 9 ? cmd[9] : 0;
            if (pl == 0 || pl >= sizeof s_path || 10 + pl > len) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            memcpy(s_path, cmd + 10, pl);
            s_path[pl] = 0;
            if (!safe_path(s_path)) { notify("{\"ota\":\"error\",\"why\":\"bad path\"}"); return 0; }
            s_kind = KIND_FILE;
        }
        s_received = 0;
        s_abort = s_end = false;
        s_state = ST_STARTING;
        /* faster link for the transfer: 15-30 ms interval, long DLE packets */
        struct ble_gap_upd_params p = { .itvl_min = 12, .itvl_max = 24, .latency = 0,
                                        .supervision_timeout = 500 };
        ble_gap_update_params(conn, &p);
        ble_gap_set_data_len(conn, 251, 2120);
        xTaskNotifyGive(s_task);
        return 0;
    }
    case 0x02: s_end = true; return 0;                 /* END */
    case 0x07:                                         /* PROGRESS u8 pct: what the host shows */
        if (len < 2) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        s_host_pct = cmd[1] > 100 ? 100 : cmd[1];
        s_host_pct_us = esp_timer_get_time();
        return 0;
    case 0x03: s_abort = true; return 0;               /* ABORT */
    case 0x06: {                                       /* RUN_APP u8 len, name ("" stops the app) */
        uint8_t nl = len > 1 ? cmd[1] : 0;
        if (nl >= sizeof s_app_req || 2 + nl > len) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        memcpy(s_app_req, cmd + 2, nl);
        s_app_req[nl] = 0;
        s_app_pending = true;
        return 0;
    }
    case 0x0D: {                                       /* WIFI_CONFIG: u8 len ssid, u8 len pass, u8 len url (VOICE.md) */
        char ssid[33] = "", pass[65] = "", url[128] = "";
        char *dst[3] = { ssid, pass, url };
        const size_t cap[3] = { sizeof ssid, sizeof pass, sizeof url };
        size_t p = 1;
        for (int k = 0; k < 3; k++) {
            if (p >= len) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            size_t l = cmd[p++];
            if (p + l > len || l >= cap[k]) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            memcpy(dst[k], cmd + p, l);
            dst[k][l] = 0;
            p += l;
        }
        if (!wifi_set(ssid, pass, url)) { notify("{\"ota\":\"wifi\",\"state\":\"error\",\"why\":\"not saved\"}"); return 0; }
        return 0;                                      /* wifi_note() reports connecting / up */
    }
    case 0x0F:                                         /* WIFI_SCAN: one {"ota":"net",…} per network, then {"ota":"scanned"} */
        if (!wifi_scan(scan_found)) notify("{\"ota\":\"error\",\"why\":\"scan running\"}");
        return 0;
    case 0x0E: {                                       /* VOICE u8 mode: 0 off, 1 stream speech */
        if (len < 2) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        if (!voice_set_mode(cmd[1])) notify("{\"ota\":\"error\",\"why\":\"no mic\"}");
        else notify("{\"ota\":\"voice\",\"mode\":%d,\"state\":\"%s\"}", voice_mode(), voice_state());
        return 0;
    }
    case 0x0C: {                                       /* LISTEN u16 ms, u8 flags (bit 0: send the audio) */
        if (len < 4) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        int ms = cmd[1] | cmd[2] << 8;
        if (ms < 100) ms = 100; else if (ms > 10000) ms = 10000;
        if (!audio_listen(ms, (cmd[3] & 1) && s_mic_subscribed, listen_done, mic_audio))
            notify("{\"ota\":\"error\",\"why\":\"sound queue full\"}");
        return 0;
    }
    case 0x0A:                                         /* SELFTEST: beep; boards with a mic say if it was heard */
        if (!audio_selftest(selftest_done)) notify("{\"ota\":\"error\",\"why\":\"sound queue full\"}");
        return 0;
    case 0x08: {                                       /* PLAY u8 len, name: /littlefs/sounds/<name>.ima */
        char nm[24], path[64];
        uint8_t nl = len > 1 ? cmd[1] : 0;
        if (nl == 0 || nl >= sizeof nm || 2 + nl > len) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        memcpy(nm, cmd + 2, nl);
        nm[nl] = 0;
        struct stat st;
        snprintf(path, sizeof path, "%s/sounds/%s.ima", FS_MOUNT_POINT, nm);
        if (stat(path, &st) != 0) { notify("{\"ota\":\"error\",\"why\":\"no sound %s\"}", nm); return 0; }
        if (!audio_play(nm)) { notify("{\"ota\":\"error\",\"why\":\"sound queue full\"}"); return 0; }
        notify("{\"ota\":\"play\",\"name\":\"%s\",\"bytes\":%ld}", nm, (long)st.st_size);
        return 0;
    }
    case 0x05: {                                       /* SELECT_PACK u8 len, name */
        uint8_t nl = len > 1 ? cmd[1] : 0;
        if (nl == 0 || nl >= sizeof s_pack_req || 2 + nl > len) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        memcpy(s_pack_req, cmd + 2, nl);
        s_pack_req[nl] = 0;
        s_pack_pending = true;                         /* gif state belongs to the main thread */
        return 0;
    }
    default:   return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
    }
}

static int data_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;
    if (s_state != ST_RECEIVING) return 0;             /* stray bytes outside a transfer */
    uint8_t tmp[520];
    uint16_t n = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, tmp, sizeof tmp, &n) != 0 || n == 0) return 0;
    s_received += n;
    if (s_received > s_size) { s_abort = true; return 0; }
    if (xStreamBufferSend(s_sb, tmp, n, 0) != n) s_abort = true;   /* host ignored the window */
    return 0;
}

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &OTA_SVC_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = &OTA_CTRL_UUID.u, .access_cb = ctrl_access, .val_handle = &s_ctrl_handle,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY },
            { .uuid = &OTA_DATA_UUID.u, .access_cb = data_access,
              .flags = BLE_GATT_CHR_F_WRITE_NO_RSP },
            { .uuid = &OTA_MIC_UUID.u, .access_cb = data_access, .val_handle = &s_mic_handle,
              .flags = BLE_GATT_CHR_F_NOTIFY },
            { 0 }
        },
    },
    { 0 }
};

const void *ota_gatt_svcs(void) { ota_prepare(); return s_svcs; }   /* boot: reserve the transfer task before WiFi */

void ota_on_connect(uint16_t conn_handle) { s_conn = conn_handle; }

void ota_on_disconnect(void)
{
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_subscribed = false;
    s_mic_subscribed = false;
    if (s_state == ST_RECEIVING || s_state == ST_FINISHING || s_state == ST_STARTING) s_abort = true;
}

void ota_on_subscribe(uint16_t attr_handle, bool on)
{
    if (attr_handle == s_ctrl_handle) s_subscribed = on;
    if (attr_handle == s_mic_handle) s_mic_subscribed = on;
}

/* LISTEN (0x0C): the audio task records and calls these. mic_audio() sends one chunk of
 * the PSND stream on the mic characteristic, retrying while the stack's buffers are full. */
static bool mic_audio(const uint8_t *d, size_t n)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || !s_mic_subscribed) return false;
    size_t max = ble_att_mtu(s_conn) > 23 ? ble_att_mtu(s_conn) - 3 : 20;   /* one notification each */
    for (size_t off = 0; off < n; ) {
        size_t k = n - off < max ? n - off : max;
        int tries = 0;
        for (;;) {
            struct os_mbuf *om = ble_hs_mbuf_from_flat(d + off, (uint16_t)k);
            if (om && ble_gatts_notify_custom(s_conn, s_mic_handle, om) == 0) break;
            if (++tries > 100) return false;              /* ~1 s of full buffers: give up */
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        off += k;
    }
    return true;
}

static void listen_done(const audio_listen_result_t *r)
{
    if (!r->has_mic) { notify("{\"ota\":\"error\",\"why\":\"no mic\"}"); return; }
    notify("{\"ota\":\"heard\",\"ms\":%d,\"samples\":%lu,\"rms\":%d,\"peak\":%d,\"freq\":%d,\"db\":%.1f,\"floor\":%d,\"sent\":%s}",
           r->ms, (unsigned long)r->samples, r->rms, r->peak, r->freq, r->db, r->floor, r->sent ? "true" : "false");
}

static void selftest_done(bool has_mic, bool heard, float db, int rq, int rt)
{
    notify("{\"ota\":\"selftest\",\"mic\":%s,\"heard\":%s,\"db\":%.1f,\"quiet\":%d,\"tone\":%d}",
           has_mic ? "true" : "false", heard ? "true" : "false", db, rq, rt);
}

bool ota_take_app_request(char *out, size_t n)
{
    if (!s_app_pending) return false;
    s_app_pending = false;
    snprintf(out, n, "%s", s_app_req);
    return true;
}

void ota_app_result(const char *name, bool ok, const char *why)
{
    if (ok) notify("{\"ota\":\"app\",\"name\":\"%s\"}", name);
    else    notify("{\"ota\":\"error\",\"why\":\"app %.20s: %.40s\"}", name, why ? why : "failed");
}

bool ota_take_pack_request(char *out, size_t n)
{
    if (!s_pack_pending) return false;
    s_pack_pending = false;
    snprintf(out, n, "%s", s_pack_req);
    return true;
}

void ota_pack_result(const char *name, bool ok)
{
    if (ok) notify("{\"ota\":\"pack\",\"name\":\"%s\"}", name);
    else    notify("{\"ota\":\"error\",\"why\":\"pack %s did not load\"}", name);
}

bool ota_file_progress(int *percent)
{
    bool host = s_host_pct >= 0 && esp_timer_get_time() - s_host_pct_us < 3000000;
    bool file = s_kind == KIND_FILE && s_state != ST_IDLE;            /* streams draw no bar */
    if (!host && !file) return false;
    if (percent) *percent = host ? s_host_pct : (s_size ? (int)((uint64_t)s_written * 100 / s_size) : 0);
    return true;
}

bool ota_active(int *percent)
{
    int st = s_state;
    if (st == ST_IDLE || s_kind != KIND_APP) return false;   /* packs, apps and streams keep the cat on screen */
    if (percent) *percent = s_size ? (int)((uint64_t)s_written * 100 / s_size) : 0;
    return true;
}

void ota_mark_valid(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "new image on %s marked valid (rollback cancelled)", run->label);
    }
}
