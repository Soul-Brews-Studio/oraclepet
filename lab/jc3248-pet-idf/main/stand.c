/* Bot stand judge: see stand.h. */
#include "stand.h"

#include <stdio.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
#define LOCK()   taskENTER_CRITICAL(&s_mux)
#define UNLOCK() taskEXIT_CRITICAL(&s_mux)
#else
#define LOCK()
#define UNLOCK()
#endif

#define US 1000000LL

static stand_pet_t s_pets[STAND_MAX];
static int s_count;

void stand_reset(void)
{
    LOCK();
    memset(s_pets, 0, sizeof s_pets);
    s_count = 0;
    UNLOCK();
}

static stand_pet_t *find_or_add(const char *name, int64_t now_us)
{
    for (int i = 0; i < s_count; i++)
        if (strcmp(s_pets[i].name, name) == 0) return &s_pets[i];
    int slot = s_count;
    if (s_count < STAND_MAX) {
        s_count++;
    } else {
        /* full: reuse the pet heard longest ago that is not on the stand and has no score */
        slot = -1;
        int64_t oldest = now_us;
        for (int i = 0; i < STAND_MAX; i++) {
            if (!s_pets[i].on && s_pets[i].best_us == 0 && s_pets[i].last_seen_us <= oldest) {
                oldest = s_pets[i].last_seen_us;
                slot = i;
            }
        }
        if (slot < 0) return NULL;      /* every slot holds a score: keep the scores */
    }
    stand_pet_t *p = &s_pets[slot];
    memset(p, 0, sizeof *p);
    strncpy(p->name, name, sizeof p->name - 1);
    p->rssi = -127;
    return p;
}

void stand_heard(const char *name, int rssi, int64_t now_us)
{
    if (!name || !name[0] || rssi >= 0 || rssi < -120) return;
    LOCK();
    stand_pet_t *p = find_or_add(name, now_us);
    if (p) {
        /* smooth: one loud or quiet packet does not move a pet on or off the stand */
        p->rssi = (p->rssi == -127) ? rssi : (3 * p->rssi + rssi) / 4;
        p->last_seen_us = now_us;
        if (p->rssi >= STAND_RSSI) {
            if (!p->near) { p->near = true; p->near_since_us = now_us; }
            p->last_strong_us = now_us;
            p->strong = true;
        }
    }
    UNLOCK();
}

void stand_tick(int64_t now_us)
{
    LOCK();
    for (int i = 0; i < s_count; i++) {
        stand_pet_t *p = &s_pets[i];
        bool lost = !p->strong || now_us - p->last_strong_us > STAND_GRACE_S * US;
        if (p->on) {
            if (lost) {                 /* visit ends at the last strong advert */
                int64_t visit = p->last_strong_us - p->near_since_us;
                if (visit > p->best_us) p->best_us = visit;
                p->on = false;
                p->near = false;
            }
        } else if (p->near) {
            if (lost) p->near = false;                       /* brushed past, never settled */
            else if (p->last_strong_us - p->near_since_us >= STAND_ENTER_S * US) p->on = true;
        }
    }
    UNLOCK();
}

int stand_current_s(const stand_pet_t *p, int64_t now_us)
{
    if (!p->on) return 0;
    (void)now_us;
    return (int)((p->last_strong_us - p->near_since_us) / US);
}

int stand_score_s(const stand_pet_t *p, int64_t now_us)
{
    int best = (int)(p->best_us / US), cur = stand_current_s(p, now_us);
    return cur > best ? cur : best;
}

int stand_top(stand_pet_t *out, int max, int64_t now_us)
{
    stand_pet_t copy[STAND_MAX];
    LOCK();
    int n = s_count;
    memcpy(copy, s_pets, sizeof(stand_pet_t) * (size_t)n);
    UNLOCK();
    /* insertion sort by score, highest first; ties: the one on the stand, then name */
    for (int i = 1; i < n; i++) {
        stand_pet_t k = copy[i];
        int j = i - 1;
        while (j >= 0) {
            int a = stand_score_s(&copy[j], now_us), b = stand_score_s(&k, now_us);
            if (a > b || (a == b && (copy[j].on || !k.on))) break;
            copy[j + 1] = copy[j];
            j--;
        }
        copy[j + 1] = k;
    }
    int m = n < max ? n : max;
    memcpy(out, copy, sizeof(stand_pet_t) * (size_t)m);
    return m;
}

static const char *short_name(const char *n)
{
    return strncmp(n, "Claude-", 7) == 0 ? n + 7 : n;
}

bool stand_line(char *out, size_t n, int64_t now_us)
{
    stand_pet_t top[1];
    if (stand_top(top, 1, now_us) == 0) return false;
    int s = stand_score_s(&top[0], now_us);
    if (s == 0 && !top[0].on) {
        snprintf(out, n, "stand: %d near", s_count);
        return true;
    }
    snprintf(out, n, "#1 %s %d:%02d%s", short_name(top[0].name), s / 60, s % 60, top[0].on ? " on" : "");
    return true;
}
