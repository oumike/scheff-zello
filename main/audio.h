#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

// Zello carries Opus. 16 kHz mono in 60 ms packets is what the phone apps use
// by default and what we advertise in the stream's codec header; the ES8311
// runs at the same rate, so nothing is resampled in either direction.
#define AUDIO_RATE        16000
#define AUDIO_FRAME       960  // samples per 60 ms Opus packet
#define AUDIO_FRAME_MS    60

// Called from the capture task with each 60 ms mic frame.
typedef void (*audio_mic_cb_t)(const int16_t *pcm, size_t samples);

esp_err_t audio_init(audio_mic_cb_t mic_cb);

// Queue decoded received audio for playback. Playback starts once a couple of
// packets are buffered (jitter cushion), or after a bounded prebuffer wait for
// shorter transmissions, and stops when the buffer drains.
void audio_play_push(const int16_t *pcm, size_t samples);
void audio_play_flush(void);
bool audio_is_playing(void);

void audio_set_volume(int percent);
// A short tone at the current volume, so a volume change can be heard.
// Skipped while voice is playing.
void audio_beep(void);
void audio_set_mic_gain(int db);

// Local mic -> speaker loopback for bring-up testing.
void audio_set_loopback(bool on);
bool audio_loopback(void);

// Peak level of the last captured and the last played frame, 0-100. Between
// them they drive the level meter in both directions.
uint8_t audio_mic_level(void);
uint8_t audio_play_level(void);
