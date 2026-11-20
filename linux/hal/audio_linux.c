// Voice through SDL's audio (PipeWire or PulseAudio on the CardputerZero,
// Core Audio in the simulator): 16 kHz mono both ways, so the system's
// default sink and source are used, whether the built-in speaker and mic or a
// USB headset. Mirrors main/audio.c: a 2 s playback buffer that prebuffers
// two frames (or 120 ms), 60 ms mic frames to the callback, and the beep.

#include <SDL.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>
#include "audio.h"
#include "esp_log.h"
#include "linux_hal.h"

static const char *TAG = "audio";

#define PLAY_BUF_SAMPLES   (2 * AUDIO_RATE)
#define PLAY_START_SAMPLES (2 * AUDIO_FRAME)
#define PLAY_START_TIMEOUT_MS 120
#define MIC_GAIN_UNITY_DB 30     // settings' default: as the source gives it
#define MIC_RETRY_MS 3000

static audio_mic_cb_t s_mic_cb;
static SDL_AudioDeviceID s_out, s_in;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static int16_t s_ring[PLAY_BUF_SAMPLES];
static size_t s_head, s_len;
static atomic_bool s_playing, s_loopback;
static atomic_int s_mic_level, s_play_level;
static _Atomic float s_volume = 1.0f, s_mic_gain = 1.0f;
static uint32_t s_beep_end;

static uint8_t peak_percent(const int16_t *pcm, size_t n)
{
    int32_t peak = 0;
    for (size_t i = 0; i < n; i++) {
        int32_t v = pcm[i] < 0 ? -(int32_t)pcm[i] : pcm[i];
        if (v > peak) peak = v;
    }
    return (uint8_t)(peak * 100 / 32768);
}

static void ring_push(const int16_t *pcm, size_t n)
{
    pthread_mutex_lock(&s_lock);
    // Dropped rather than blocking the decoder if somehow way behind.
    if (n > PLAY_BUF_SAMPLES - s_len) n = PLAY_BUF_SAMPLES - s_len;
    for (size_t i = 0; i < n; i++) s_ring[(s_head + s_len + i) % PLAY_BUF_SAMPLES] = pcm[i];
    s_len += n;
    pthread_mutex_unlock(&s_lock);
}

static size_t ring_pop(int16_t *out, size_t n)
{
    pthread_mutex_lock(&s_lock);
    if (n > s_len) n = s_len;
    for (size_t i = 0; i < n; i++) out[i] = s_ring[(s_head + i) % PLAY_BUF_SAMPLES];
    s_head = (s_head + n) % PLAY_BUF_SAMPLES;
    s_len -= n;
    pthread_mutex_unlock(&s_lock);
    return n;
}

static size_t ring_len(void)
{
    pthread_mutex_lock(&s_lock);
    size_t n = s_len;
    pthread_mutex_unlock(&s_lock);
    return n;
}

static void scale(int16_t *pcm, size_t n, float g)
{
    if (g == 1.0f) return;
    for (size_t i = 0; i < n; i++) {
        float v = pcm[i] * g;
        pcm[i] = (int16_t)(v > 32767.f ? 32767 : v < -32768.f ? -32768 : v);
    }
}

static SDL_AudioDeviceID open_dev(bool capture)
{
    SDL_AudioSpec want = {0}, have;
    want.freq = AUDIO_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = 512;
    SDL_AudioDeviceID id = SDL_OpenAudioDevice(NULL, capture, &want, &have, 0);
    if (id) SDL_PauseAudioDevice(id, 0);
    return id;
}

// Paced by SDL's queue: keeps about three frames queued on the device, so a
// frame's worth of silence pads any gap the way the I2S boards do.
static void *play_thread(void *arg)
{
    (void)arg;
    int16_t frame[AUDIO_FRAME];
    bool waiting = false;
    uint32_t queued_since = 0;
    for (;;) {
        if (SDL_GetQueuedAudioSize(s_out) > 2 * sizeof(frame)) {
            usleep(10 * 1000);
            continue;
        }
        size_t got = 0;
        if (!atomic_load(&s_playing)) {
            size_t queued = ring_len();
            uint32_t now = hal_now_ms();
            if (queued && !waiting) {
                queued_since = now;
                waiting = true;
            }
            if (queued >= PLAY_START_SAMPLES || (queued && now - queued_since >= PLAY_START_TIMEOUT_MS))
                atomic_store(&s_playing, true);
            if (!queued || atomic_load(&s_playing)) waiting = false;
        }
        if (atomic_load(&s_playing)) {
            got = ring_pop(frame, AUDIO_FRAME);
            if (got < AUDIO_FRAME) atomic_store(&s_playing, false);  // drained: re-prebuffer
        }
        memset(frame + got, 0, (AUDIO_FRAME - got) * sizeof(int16_t));
        atomic_store(&s_play_level, got ? peak_percent(frame, got) : 0);
        scale(frame, AUDIO_FRAME, s_volume);
        SDL_QueueAudio(s_out, frame, sizeof(frame));
    }
    return NULL;
}

// Opens the default source when there is one (a headset may come later) and
// hands on whole 60 ms frames.
static void *mic_thread(void *arg)
{
    (void)arg;
    int16_t frame[AUDIO_FRAME];
    size_t have = 0;
    uint32_t last_try = 0;
    bool told = false;
    for (;;) {
        if (!s_in) {
            uint32_t now = hal_now_ms();
            if (last_try && now - last_try < MIC_RETRY_MS) {
                usleep(100 * 1000);
                continue;
            }
            last_try = now;
            s_in = open_dev(true);
            if (!s_in) {
                if (!told) ESP_LOGW(TAG, "no microphone: %s (plug in a headset)", SDL_GetError());
                told = true;
                continue;
            }
            ESP_LOGI(TAG, "microphone open");
            told = false;
            have = 0;
        }
        if (SDL_GetAudioDeviceStatus(s_in) == SDL_AUDIO_STOPPED) {
            ESP_LOGW(TAG, "microphone gone");
            SDL_CloseAudioDevice(s_in);
            s_in = 0;
            continue;
        }
        Uint32 n = SDL_DequeueAudio(s_in, frame + have, (Uint32)((AUDIO_FRAME - have) * sizeof(int16_t)));
        have += n / sizeof(int16_t);
        if (have < AUDIO_FRAME) {
            usleep(10 * 1000);
            continue;
        }
        have = 0;
        scale(frame, AUDIO_FRAME, s_mic_gain);
        atomic_store(&s_mic_level, peak_percent(frame, AUDIO_FRAME));
        if (atomic_load(&s_loopback)) ring_push(frame, AUDIO_FRAME);
        else if (s_mic_cb) s_mic_cb(frame, AUDIO_FRAME);
    }
    return NULL;
}

esp_err_t audio_init(audio_mic_cb_t mic_cb)
{
    s_mic_cb = mic_cb;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        ESP_LOGE(TAG, "SDL audio: %s", SDL_GetError());
        return ESP_FAIL;
    }
    s_out = open_dev(false);
    if (!s_out) {
        ESP_LOGE(TAG, "no audio output: %s", SDL_GetError());
        return ESP_FAIL;
    }
    pthread_t t;
    if (pthread_create(&t, NULL, play_thread, NULL) != 0) return ESP_FAIL;
    pthread_detach(t);
    if (pthread_create(&t, NULL, mic_thread, NULL) != 0) return ESP_FAIL;
    pthread_detach(t);
    ESP_LOGI(TAG, "audio running at %d Hz mono, %d ms frames (%s)", AUDIO_RATE, AUDIO_FRAME_MS,
             SDL_GetCurrentAudioDriver());
    return ESP_OK;
}

void audio_play_push(const int16_t *pcm, size_t samples) { ring_push(pcm, samples); }

void audio_play_flush(void)
{
    pthread_mutex_lock(&s_lock);
    s_head = s_len = 0;
    pthread_mutex_unlock(&s_lock);
}

bool audio_is_playing(void) { return atomic_load(&s_playing); }
bool audio_has_mic(void) { return s_in != 0; }

void audio_set_volume(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    // About 40 dB across the slider, as a codec's volume would be.
    s_volume = percent ? powf(10.0f, (percent - 100) * 0.4f / 20.0f) : 0.0f;
}

#define BEEP_HZ   880
#define BEEP_MS   150
#define BEEP_RAMP 160
#define BEEP_PEAK 8000

void audio_beep(void)
{
    // Voice playing is left alone; an earlier beep still sounding is replaced.
    if (!s_out || (atomic_load(&s_playing) && (int32_t)(hal_now_ms() - s_beep_end) > 0)) return;
    enum { N = AUDIO_RATE * BEEP_MS / 1000 };
    static int16_t tone[N];
    if (!tone[1]) {
        for (int i = 0; i < N; i++) {
            float env = 1.0f;
            if (i < BEEP_RAMP) env = (float)i / BEEP_RAMP;
            if (N - i < BEEP_RAMP) env = (float)(N - i) / BEEP_RAMP;
            tone[i] = (int16_t)(BEEP_PEAK * env * sinf(2.0f * (float)M_PI * BEEP_HZ * i / AUDIO_RATE));
        }
    }
    audio_play_flush();
    ring_push(tone, N);
    s_beep_end = hal_now_ms() + BEEP_MS + 2 * AUDIO_FRAME_MS;
}

void audio_set_mic_gain(int db)
{
    s_mic_gain = powf(10.0f, (db - MIC_GAIN_UNITY_DB) / 20.0f);
}

void audio_set_loopback(bool on)
{
    atomic_store(&s_loopback, on);
    if (!on) audio_play_flush();
}

bool audio_loopback(void) { return atomic_load(&s_loopback); }
uint8_t audio_mic_level(void) { return (uint8_t)atomic_load(&s_mic_level); }
uint8_t audio_play_level(void) { return (uint8_t)atomic_load(&s_play_level); }
