/*
 * dbk.h — DBK link v1, the pet (receiver) side. esp32-oracle issue #32.
 *
 * A DustBoy Kit ("kit") sends its PM reading two ways:
 *   - ESP-NOW broadcast every 5 s, ASCII JSON {"t":"dbk","v":1,"id":"173e5c","pm25":12,...},
 *     plus the same JSON UNICAST to every pet it knows, with "to":"<pet id>" and "seq":<n>;
 *   - a non-connectable BLE advert, manufacturer data 0xFFFF 'D','B',1, pm25/pm10/pm1 u16 LE,
 *     band u8 (ble.c parses it and calls dbk_on_reading()).
 *
 * The pet listens (channel rules below), keeps the latest reading per kit (up to 4), answers
 * a unicast reading with {"t":"ack","v":1,"id":"<pet id>","seq":<n>} to the kit MAC, and every
 * 5 s broadcasts its own status {"t":"pet",...}. Every pet and kit heard goes into a peer table
 * (status sync), so pet A knows pet B's state.
 *
 * Channel rules: associated to an AP -> stay on the AP channel, never hop. Not associated ->
 * dwell 5.5 s per channel, order 1,6,11,2,3,4,5,7,8,9,10,12,13; lock on the first valid "dbk"
 * frame (tuning to the frame's "ch" if it names one); hop again after 30 s of silence. The
 * channel is never set while a scan or a join owns the radio (wifi_busy()), and is put back
 * when the radio comes back elsewhere. While locked and not joined, the saved network is
 * retried every 60 s instead of 5 s. Power save off (WIFI_PS_NONE) or broadcasts are missed.
 *
 * BLE: a 'D','B' advert is used only once the "DBK-xxxxxx" name has been seen from that
 * address (the BLE MAC differs from the WiFi MAC the ESP-NOW id comes from), so the kit
 * must put its name in the advert itself (a non-connectable advert has no scan response).
 *
 * Threading: the ESP-NOW callbacks and ble.c's scan callback only copy into a queue / a table
 * under a spinlock. All parsing, sending, hopping and logging run on the "dbk" task. The main
 * loop reads a copy with dbk_view() and owns every HUD/GIF call.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DBK_MAX_KITS   4
#define DBK_BAND_GOOD  0
#define DBK_BAND_FAIR  1
#define DBK_BAND_POOR  2

enum { DBK_SRC_ESPNOW = 'E', DBK_SRC_BLE = 'B' };

typedef struct {
    char    id[8];        /* "173e5c" */
    int     pm25, pm10, pm1;
    int     band;         /* DBK_BAND_* */
    int     rssi;
    char    src[3];       /* "E" ESP-NOW, "B" BLE, "EB" both heard in the last 10 s */
    int64_t age_us;       /* since the latest reading */
} dbk_kit_view_t;

typedef struct {
    int            nkits;            /* live kits (reading < 60 s old), freshest first */
    dbk_kit_view_t kit[DBK_MAX_KITS];
    int            pets;             /* other pets heard in the last 30 s */
    bool           kit_ok;           /* a kit heard in the last 30 s (any source) */
    int            ch;               /* current WiFi channel (0 = radio off) */
    bool           locked;           /* hopping stopped: AP channel or a kit found */
    uint32_t       rx;               /* valid ESP-NOW "dbk" frames received (BLE not counted) */
} dbk_view_t;

/* Start the radio (STA, no join when no network is saved), ESP-NOW and the dbk task.
 * Call after bleInit() (NVS) and wifi_init(). name = "Claude-AD48", pack = the pack shown. */
void dbk_init(const char *name, const char *pack);

/* A reading from BLE (ble.c scan callback, NimBLE host task). Cheap: copies under a lock. */
void dbk_on_reading(const char *id, int pm25, int pm10, int pm1, int band, int rssi, char src);

/* Main loop: what the pet tells other boards in its status frame. */
void dbk_set_state(const char *state);   /* "idle" | "busy" | "attention" | "sleep" */
void dbk_set_pack(const char *pack);

/* Main loop: a copy of the current state for the HUD. */
void dbk_view(dbk_view_t *out);

/* pm25 -> band, per the spec: <=25 good, <=50 fair, else poor. */
int         dbk_band_of(int pm25);
const char *dbk_band_name(int band);

#ifdef __cplusplus
}
#endif
