/* improv.h — Improv Wi-Fi over BLE (improv-wifi.com/ble): any Improv client sets the pet's WiFi. */
#pragma once
#include <stdint.h>
#include "host/ble_gatt.h"
#include "host/ble_uuid.h"

#ifdef __cplusplus
extern "C" {
#endif

const struct ble_gatt_svc_def *improv_gatt_svcs(void);
const ble_uuid128_t *improv_uuid(void);   /* advertised so Improv clients find the pet */
uint8_t improv_state(void);               /* for the 0x4677 service data in the advertisement */
void improv_wifi_note(const char *state); /* wifi.c state changes ("up 192.168.1.23", ...) */
void improv_on_disconnect(void);

#ifdef __cplusplus
}
#endif
