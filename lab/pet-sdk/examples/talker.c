// talker: the pet speaks. A tap says hello (OraclePet makes the voice on the Mac and
// sends it back), someone starting to wait gets a two-note chime and "someone needs
// you", and answering everyone gets a little rising tune.
#include "pet.h"

static int last_waiting;

PET_EXPORT("on_start") void on_start(void)
{
    pet_status("app: talker");
    pet_caption("tap me");
    pet_say("Hello, I can talk now");
}

PET_EXPORT("on_tap") void on_tap(int x, int y)
{
    pet_react();
    pet_say("Hello from the pet");
}

PET_EXPORT("on_fleet") void on_fleet(int total, int running, int waiting)
{
    pet_show(waiting ? PET_ATTENTION : running ? PET_BUSY : total ? PET_IDLE : PET_SLEEP);
    if (waiting > last_waiting) {
        pet_tone(880, 120);
        pet_tone(660, 160);
        pet_say(waiting == 1 ? "Someone needs you" : "Some agents need you");
        pet_caption("needs you");
    } else if (last_waiting > 0 && waiting == 0) {
        pet_tone(523, 90);
        pet_tone(659, 90);
        pet_tone(784, 140);
        pet_caption("all answered");
    }
    last_waiting = waiting;
}
