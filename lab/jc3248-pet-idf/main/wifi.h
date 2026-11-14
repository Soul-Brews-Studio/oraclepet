/* wifi.h — WiFi for the voice stream (VOICE.md). Credentials from BLE, kept in NVS. */
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*wifi_note_cb)(const char *state);   /* "connecting", "up 192.168.1.23", ... */

void        wifi_init(wifi_note_cb cb);             /* connect with the saved network, if any */
bool        wifi_set(const char *ssid, const char *pass, const char *url);   /* save and connect */
const char *wifi_state(void);
bool        wifi_is_up(void);
const char *wifi_url(void);
/* Scan for networks (2.4 GHz, what the pet can join); each one goes to found(), strongest first.
 * Runs on its own task; false if one is already running. */
typedef void (*wifi_found_cb)(const char *ssid, int rssi, int channel, bool open);
bool        wifi_scan(wifi_found_cb found);

#ifdef __cplusplus
}
#endif
