// hello: the smallest pet app. Counts taps under the pet and celebrates every 5th.
#include "pet.h"

static int taps;

PET_EXPORT("on_start") void on_start(void)
{
    pet_caption("hello from wasm");
    pet_status("app: hello");
    pet_log("hello started");
}

PET_EXPORT("on_tap") void on_tap(int x, int y)
{
    char b[24]; int at = 0;
    taps++;
    pet_put_str(b, &at, "taps: ");
    pet_put_int(b, &at, taps);
    pet_caption(b);
    pet_sound(PET_BLIP_UP);
    if (taps % 5 == 0) pet_react();
}
