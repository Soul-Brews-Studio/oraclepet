/* voice.h — the pet's microphone to the voice server over a WebSocket (VOICE.md). */
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void voice_init(const char *pet_name, const char *project, const char *fw);
bool voice_set_mode(int mode);          /* 0 off, 1 stream speech (saved in NVS); false = no mic */
int  voice_mode(void);
void voice_wifi_up(const char *url);    /* from wifi.c */
void voice_wifi_down(void);
const char *voice_state(void);          /* "off", "waiting for wifi", "connected", ... */

#ifdef __cplusplus
}
#endif
