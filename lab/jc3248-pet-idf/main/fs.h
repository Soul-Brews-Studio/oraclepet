/*
 * fs.h — jc3248-pet-idf STEP 5: LittleFS asset filesystem.
 *
 * Mounts the "storage" partition (partitions.csv, 3MB littlefs) at /littlefs via
 * the joltwallet/littlefs managed component (esp_littlefs / esp_vfs). The bufo
 * GIF pack, flashed from the project ../data dir by littlefs_create_partition_image
 * (see main/CMakeLists.txt), is then reachable through the standard C VFS:
 *
 *     FILE *f = fopen("/littlefs/characters/bufo/idle_0.gif", "rb");
 *
 * Step 6's AnimatedGIF decode loop reads frames through exactly these POSIX calls,
 * the IDF analogue of the Arduino app's LittleFS.open(...) callbacks (pet.cpp).
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Base path the storage partition is mounted at. Prefix every asset path with
 * this (e.g. FS_MOUNT_POINT "/characters/bufo/heart.gif"). */
#define FS_MOUNT_POINT      "/littlefs"

/* Partition label — MUST match the "storage" row in partitions.csv and the
 * first arg of littlefs_create_partition_image(storage ...) in CMakeLists.txt. */
#define FS_PARTITION_LABEL  "storage"

/*
 * Mount the LittleFS "storage" partition at /littlefs.
 *
 * format_if_mount_failed = true: a blank/corrupt partition is formatted into a
 * clean empty FS (mirrors the Arduino app's LittleFS.begin(formatOnFail=true)).
 * On a fresh image-flashed device the mount succeeds with the assets present;
 * on a never-written partition you get an empty but usable FS.
 *
 * Logs the partition's total/used bytes on success. Returns ESP_OK on mount;
 * on failure returns the esp_littlefs error and leaves nothing mounted (the
 * caller should log + continue — assets simply won't be available).
 *
 * Idempotent-ish: calling twice returns ESP_ERR_INVALID_STATE from the
 * underlying register (already-registered base path) — mount once at boot.
 */
esp_err_t fs_mount(void);

/* Unmount /littlefs and unregister the VFS. Returns ESP_OK, or an error if it
 * was never mounted. (Not normally needed — the FS lives for the whole session.) */
esp_err_t fs_unmount(void);

/* True if fs_mount() has succeeded and not been unmounted. */
bool fs_is_mounted(void);

#ifdef __cplusplus
}
#endif
