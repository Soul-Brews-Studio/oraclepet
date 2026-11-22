/*
 * qemu_demo.h — qemu board only: a demo DustBoy Kit feed. ESP-NOW and BLE cannot run in QEMU,
 * so this task calls dbk_on_reading() with sample readings and the PM2.5 HUD has something to show.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Start the demo feed task (one reading per kit every 5 s). */
void qemu_demo_start(void);

#ifdef __cplusplus
}
#endif
