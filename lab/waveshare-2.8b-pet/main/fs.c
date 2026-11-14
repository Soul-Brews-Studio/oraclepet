/*
 * fs.c — jc3248-pet-idf STEP 5: LittleFS asset filesystem.
 *
 * Mounts the "storage" partition at /littlefs using the joltwallet/littlefs
 * managed component (header: esp_littlefs.h, fn: esp_vfs_littlefs_register).
 * The bufo GIF pack is flashed into this partition by the
 * littlefs_create_partition_image(storage ../data ...) call in main/CMakeLists.txt.
 *
 * This is the IDF-side equivalent of the Arduino app's LittleFS.begin(): once
 * mounted, step 6 opens frames with plain fopen("/littlefs/characters/...","rb").
 */
#include "fs.h"

#include "esp_log.h"
#include "esp_littlefs.h"   /* managed component: joltwallet/littlefs */

static const char *TAG = "fs";

static bool s_mounted = false;

esp_err_t fs_mount(void)
{
    if (s_mounted) {
        ESP_LOGW(TAG, "already mounted at %s", FS_MOUNT_POINT);
        return ESP_OK;
    }

    /* format_if_mount_failed mirrors the Arduino LittleFS.begin(formatOnFail=true):
     * a blank/corrupt partition is reformatted into a clean empty FS instead of
     * failing the boot. The image flashed by littlefs_create_partition_image
     * normally mounts cleanly with the assets already present. */
    esp_vfs_littlefs_conf_t conf = {
        .base_path              = FS_MOUNT_POINT,
        .partition_label        = FS_PARTITION_LABEL,
        .format_if_mount_failed = true,
        .dont_mount             = false,
    };

    esp_err_t e = esp_vfs_littlefs_register(&conf);
    if (e != ESP_OK) {
        if (e == ESP_FAIL) {
            ESP_LOGE(TAG, "mount/format failed for partition '%s'", FS_PARTITION_LABEL);
        } else if (e == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "no '%s' partition — check partitions.csv / CONFIG_PARTITION_TABLE_CUSTOM",
                     FS_PARTITION_LABEL);
        } else {
            ESP_LOGE(TAG, "esp_vfs_littlefs_register: %s", esp_err_to_name(e));
        }
        return e;
    }

    size_t total = 0, used = 0;
    esp_err_t info = esp_littlefs_info(FS_PARTITION_LABEL, &total, &used);
    if (info == ESP_OK) {
        ESP_LOGI(TAG, "mounted '%s' at %s — %u/%u bytes used",
                 FS_PARTITION_LABEL, FS_MOUNT_POINT,
                 (unsigned)used, (unsigned)total);
    } else {
        ESP_LOGI(TAG, "mounted '%s' at %s (info unavailable: %s)",
                 FS_PARTITION_LABEL, FS_MOUNT_POINT, esp_err_to_name(info));
    }

    s_mounted = true;
    return ESP_OK;
}

esp_err_t fs_unmount(void)
{
    if (!s_mounted) {
        ESP_LOGW(TAG, "not mounted");
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t e = esp_vfs_littlefs_unregister(FS_PARTITION_LABEL);
    if (e == ESP_OK) {
        s_mounted = false;
        ESP_LOGI(TAG, "unmounted '%s'", FS_PARTITION_LABEL);
    } else {
        ESP_LOGE(TAG, "esp_vfs_littlefs_unregister: %s", esp_err_to_name(e));
    }
    return e;
}

bool fs_is_mounted(void)
{
    return s_mounted;
}
