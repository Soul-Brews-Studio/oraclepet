/*
 * touch_qemu.c — qemu board: QEMU has no touch controller, so touch is a stub that never fires.
 */
#include "touch.h"

esp_err_t touch_init(void) { return ESP_OK; }

bool touch_poll(uint16_t *x, uint16_t *y)
{
    (void)x;
    (void)y;
    return false;
}
