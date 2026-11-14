/*
 * audio.h — waveshare-2.8b-pet stub
 *
 * The 2.8B has a passive buzzer via TCA9554 EXIO7, not an I2S amp.
 * Full buzzer support can be wired later via the TCA9554 handle exposed
 * by display.c. For now all audio calls are no-ops.
 */
#pragma once

void audio_init(void);
void audio_ribbit(void);
