#pragma once
/*
 * Bot stand: this pet judges how long other pets stay next to it.
 *
 * The pet already scans for the host's time beacon (ble.c). Every advert named
 * "Claude-XXXX" (another pet, not this one) goes to stand_heard() with its RSSI.
 * A pet whose smoothed signal stays at or above STAND_RSSI for STAND_ENTER_S is
 * "on the stand"; it leaves after STAND_GRACE_S without a strong advert. The
 * score is the longest single visit (best), plus the visit running now.
 *
 * Scanning pauses while a host is connected (ble.c), so the judge only counts
 * while nothing is linked to it. Pure logic, no IDF calls: host-testable.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STAND_MAX      12      /* pets remembered */
#define STAND_RSSI     (-60)   /* dBm, smoothed: at or above = close enough */
#define STAND_ENTER_S  10      /* strong for this long before a visit starts */
#define STAND_GRACE_S  30      /* no strong advert for this long ends the visit */

typedef struct {
    char    name[20];          /* "Claude-B214" */
    int     rssi;              /* smoothed dBm */
    bool    on;                /* on the stand now */
    bool    near;              /* strong adverts running, maybe not settled yet */
    bool    strong;            /* ever heard at or above STAND_RSSI */
    int64_t near_since_us;     /* first strong advert of this run (valid while near) */
    int64_t last_strong_us;    /* last advert at or above STAND_RSSI */
    int64_t last_seen_us;
    int64_t best_us;           /* longest finished visit */
} stand_pet_t;

/* An advert from another pet (any task; locks inside). */
void stand_heard(const char *name, int rssi, int64_t now_us);

/* Advance visits: start, end, keep best. Call from the main loop. */
void stand_tick(int64_t now_us);

/* Seconds of the visit running now (0 if off the stand), and the score: max(best, current). */
int  stand_current_s(const stand_pet_t *p, int64_t now_us);
int  stand_score_s(const stand_pet_t *p, int64_t now_us);

/* Copy up to max pets, highest score first. Returns the count. */
int  stand_top(stand_pet_t *out, int max, int64_t now_us);

/* One HUD line: "1 B214 3:12 on" / "stand: nobody", false when nothing was ever heard. */
bool stand_line(char *out, size_t n, int64_t now_us);

/* Test hook: forget everyone. */
void stand_reset(void);

#ifdef __cplusplus
}
#endif
