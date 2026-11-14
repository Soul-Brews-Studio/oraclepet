// oracle-tour: the seven oracle cats take turns on the pet, 20 s each; a tap moves on.
#include "pet.h"

static const char *ORACLES[] = { "iris", "mother", "neo", "atlas", "calliope", "volt", "pulse" };
#define N (int)(sizeof ORACLES / sizeof ORACLES[0])
static int at = -1, next_ms;

static void next(int now)
{
    for (int tries = 0; tries < N; tries++) {        // skip packs that are not on this pet
        at = (at + 1) % N;
        if (pet_pack(ORACLES[at])) break;
    }
    pet_caption(ORACLES[at]);
    next_ms = now + 20000;
}

PET_EXPORT("on_start") void on_start(void) { pet_status("app: oracle tour"); next(pet_millis()); }
PET_EXPORT("on_tick")  void on_tick(int ms) { if (ms >= next_ms) next(ms); }
PET_EXPORT("on_tap")   void on_tap(int x, int y) { pet_react(); next(pet_millis()); }
