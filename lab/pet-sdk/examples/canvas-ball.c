// canvas-ball: canvas mode. The app draws the whole screen above the HUD: a ball bouncing
// off the walls, a tap counter, and a tap moves the ball to your finger.
#include "pet.h"

static int x = 60, y = 80, dx = 7, dy = 5, r = 18, taps;

static void draw(void)
{
    int w = pet_width(), h = pet_height();
    char b[24]; int at = 0;
    pet_clear(pet_rgb(10, 14, 30));
    pet_rect(0, 0, w, 4, pet_rgb(242, 181, 68));
    pet_line(0, h - 1, w - 1, h - 1, pet_rgb(60, 70, 90));
    pet_circle(x, y, r, pet_rgb(255, 120, 60));
    pet_circle(x - 6, y - 6, 5, pet_rgb(255, 210, 170));
    pet_put_str(b, &at, "taps ");
    pet_put_int(b, &at, taps);
    pet_text(10, 14, 2, pet_rgb(255, 255, 255), b);
    pet_text(10, h - 22, 1, pet_rgb(140, 150, 170), "canvas mode: tap to move");
}

PET_EXPORT("on_start") void on_start(void)
{
    pet_canvas(1);
    pet_status("app: canvas-ball");
    draw();
}

PET_EXPORT("on_tick") void on_tick(int ms)
{
    int w = pet_width(), h = pet_height();
    x += dx; y += dy;
    if (x < r || x > w - r) { dx = -dx; x += dx; pet_sound(PET_BLIP_DOWN); }
    if (y < r + 4 || y > h - r) { dy = -dy; y += dy; }
    draw();
}

PET_EXPORT("on_tap") void on_tap(int tx, int ty)
{
    taps++;
    x = tx; y = ty;
    pet_sound(PET_BLIP_UP);
    draw();
}
