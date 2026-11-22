#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// A USB audio adapter (UAC 1.0, e.g. a C-Media sound card) on the Tanmatsu's
// or Tab5's USB-A host port. It adds a microphone and a headphone/speaker
// output (on the Tab5, in place of the built-in ones); both follow hot-plugging. Frames are AUDIO_FRAME mono samples at AUDIO_RATE: the
// adapter's native rate (16 or 48 kHz) is converted here.

esp_err_t usb_audio_init(void);

bool usb_audio_mic_ready(void);
bool usb_audio_speaker_ready(void);

// Reads one frame from the adapter's microphone; false if none is plugged in
// or no full frame arrived in time.
bool usb_audio_read(int16_t *frame);
// Queues one frame for the adapter's output; blocks while its buffer is full.
bool usb_audio_write(const int16_t *frame);

void usb_audio_set_volume(int percent);
