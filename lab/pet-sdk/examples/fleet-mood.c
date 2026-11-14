// fleet-mood: the app decides what the fleet means. Waiting beats working, a quiet fleet
// naps after a minute, and answering the last waiting agent gets a celebration.
#include "pet.h"

static int last_waiting, quiet_since;

PET_EXPORT("on_start") void on_start(void) { pet_status("app: fleet mood"); quiet_since = pet_millis(); }

PET_EXPORT("on_fleet") void on_fleet(int total, int running, int waiting)
{
    char b[24]; int at = 0;
    int now = pet_millis();
    if (waiting > 0) {
        pet_show(PET_ATTENTION);
        pet_put_int(b, &at, waiting); pet_put_str(b, &at, waiting == 1 ? " needs you" : " need you");
    } else if (running > 0) {
        pet_show(PET_BUSY);
        pet_put_int(b, &at, running); pet_put_str(b, &at, " working");
    } else {
        if (running == 0 && total == 0 && now - quiet_since > 60000) pet_show(PET_SLEEP); else pet_show(PET_IDLE);
        pet_put_str(b, &at, total ? "all quiet" : "zzz");
    }
    if (waiting || running) quiet_since = now;
    if (last_waiting > 0 && waiting == 0) { pet_react(); pet_sound(PET_BLIP_UP); }   // you answered them all
    last_waiting = waiting;
    pet_caption(b);
}
