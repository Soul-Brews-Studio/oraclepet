/* common.h — the pet's WebRTC voice lab. */
#pragma once

#include "esp_capture.h"
#include "esp_webrtc.h"
#include "media_sys.h"
#include "network.h"
#include "sys_state.h"

#ifdef __cplusplus
extern "C" {
#endif

int start_webrtc(void);
void query_webrtc(void);
int stop_webrtc(void);
/* Text to the browser over the data channel (dropped when none is open). */
void pet_say(const char *text);

#ifdef __cplusplus
}
#endif
