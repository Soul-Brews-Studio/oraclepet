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
/* Bring the radio up in STA mode WITHOUT joining anything when no network is saved (ESP-NOW
 * needs a started driver). Idempotent; true once the driver is started. */
bool        wifi_radio_start(void);
/* true while a scan or a join owns the radio (channels move): do not set the channel then */
bool        wifi_busy(void);
/* joined to the AP (from WIFI_EVENT_STA_CONNECTED, before DHCP): stay on the AP channel */
bool        wifi_assoc(void);
/* dbk.c: locked on a kit while not joined -> retry the saved network every 60 s, not 5 s */
void        wifi_set_slow_retry(bool slow);
bool        wifi_has_saved(void);   /* a network is saved: STA keeps trying to join it */

#ifdef __cplusplus
}
#endif
