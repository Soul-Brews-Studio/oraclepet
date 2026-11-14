// speaker-test: is the pet's speaker alive? A ribbit at start, then a blip every 3 s
// (up, down, up...) for 15 s, with the caption saying what should be playing.
#include "pet.h"

static int started, n;

PET_EXPORT("on_start") void on_start(void)
{
    started = pet_millis();
    pet_caption("ribbit?");
    pet_status("app: speaker test");
    pet_sound(PET_RIBBIT);
}

PET_EXPORT("on_tick") void on_tick(int ms)
{
    if (n >= 5 || ms - started < (n + 1) * 3000) return;
    n++;
    pet_caption(n % 2 ? "blip up?" : "blip down?");
    pet_sound(n % 2 ? PET_BLIP_UP : PET_BLIP_DOWN);
    if (n == 5) pet_caption("speaker test done");
}
