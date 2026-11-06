#pragma once

// Zello Channels API client: one WebSocket to wss://zello.io/ws (consumer) or
// wss://zellowork.io/ws/<network> (Zello Work), a logon that joins one channel,
// and Opus audio streams in both directions.
//
// Protocol, as far as we use it:
//   -> {"command":"logon","seq":1,"auth_token":..,"username":..,"password":..,
//       "channel":..}
//   <- {"seq":1,"success":true,"refresh_token":..}
//   <- {"command":"on_channel_status","channel":..,"status":"online",
//       "users_online":N}
//   -> {"command":"start_stream","seq":2,"type":"audio","codec":"opus",
//       "codec_header":<base64>,"packet_duration":60}
//   <- {"seq":2,"success":true,"stream_id":N}
//   -> binary: [0x01][stream_id:4 BE][packet_id:4 BE][opus payload]
//   -> {"command":"stop_stream","seq":3,"stream_id":N}
//   <- {"command":"on_stream_start",..,"stream_id":N,"from":..} then binary
//      packets, then {"command":"on_stream_stop","stream_id":N}

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    ZELLO_OFFLINE,     // no socket, and not trying
    ZELLO_CONNECTING,  // TCP/TLS and the WebSocket handshake
    ZELLO_LOGGING_IN,  // logon sent, waiting for the channel to come online
    ZELLO_ONLINE,      // in the channel, ready to talk
    ZELLO_FAILED,      // the server rejected us; `error` says why
} zello_state_t;

typedef struct {
    zello_state_t state;
    char channel[65];
    uint32_t users_online;
    bool ptt;        // we hold a transmit stream
    bool starting;   // start_stream sent, no stream_id yet
    bool receiving;  // someone else is talking
    char talker[64];
    char error[128];  // last failure, for the UI and the console
    uint32_t rx_packets;
    uint32_t tx_packets;
} zello_status_t;

// Creates the codec, the audio queues and their tasks. Does not connect.
esp_err_t zello_init(void);

void zello_get_status(zello_status_t *out);
zello_state_t zello_state(void);

// Connects with the current settings, replacing any existing session. Returns
// ESP_ERR_INVALID_ARG when the account or channel is not configured yet.
esp_err_t zello_connect(void);
void zello_disconnect(void);

// Boot login after Wi-Fi is ready. Does nothing if a manual connection action
// has already superseded it, or automatic login has since been disabled.
esp_err_t zello_auto_connect(void);

// Applies an account or network change: reconnects if the account is complete,
// otherwise just goes offline.
void zello_reconfigure(void);

// Saves `name` as the channel and re-logs on. An empty name only saves.
esp_err_t zello_set_channel(const char *name);

// Transmit. Starting is asynchronous - the server has to allocate the stream -
// so status.starting is true until status.ptt is.
void zello_set_ptt(bool on);
bool zello_ptt(void);

// Handed to audio_init(); frames are dropped unless we are transmitting.
void zello_mic_frame(const int16_t *pcm, size_t samples);

// Sends a channel text message. ESP_ERR_INVALID_STATE when not in a channel.
// A refusal from the server shows up in the log.
//   -> {"command":"send_text_message","seq":N,"channel":..,"text":..}
//   <- {"command":"on_text_message","channel":..,"from":..,"text":..}
esp_err_t zello_send_text(const char *text);

// The recent channel activity (joins, talkers, text messages, errors) as
// newline-separated lines, oldest first. A buffer of ZELLO_LOG_MAX holds all
// of it.
#define ZELLO_LOG_MAX 8192
void zello_get_log(char *out, size_t size);

// Registers a callback run whenever the status or the log changes, so the UI
// can redraw without polling hard. Called from the client's own tasks.
void zello_set_changed_cb(void (*cb)(void));
