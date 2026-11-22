// Voice through SDL's audio (PipeWire or PulseAudio on the CardputerZero,
// Core Audio in the simulator): 16 kHz mono both ways, so the system's
// default sink is used, whether the built-in speaker or a USB headset, and the
// mic is a USB one whenever one is plugged in, else the default source.
// Mirrors main/audio.c: a 2 s playback buffer that prebuffers
// two frames (or 120 ms), 60 ms mic frames to the callback, and the beep.
//
// The volume is the system's: the slider and volume keys set the ALSA
// "default" mixer (PipeWire's or PulseAudio's when one runs, else the
// codec's), the app starts at whatever level the system is at, and changes
// made outside the app show up on the slider. Without ALSA (the macOS
// simulator) the app scales its own stream instead.
//
// The mic gain sets the codec's analog preamp (its "PGA" controls, 0-42 dB in
// 3 dB steps, as main/audio.c does on the ESP32 boards) and, on top of that,
// a digital gain that is unity at the default 30 dB.

#include <SDL.h>
#include <math.h>
#include <stdio.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>
#include "audio.h"
#include "esp_log.h"
#include "linux_hal.h"
#include "settings.h"
#if HAVE_ALSA_MIXER
#include <alsa/asoundlib.h>
#endif

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

// ---- the system mixer ------------------------------------------------------

#if HAVE_ALSA_MIXER

#define MIXER_POLL_MS 500

static pthread_mutex_t s_mixer_lock = PTHREAD_MUTEX_INITIALIZER;
static snd_mixer_t *s_mixer;
static snd_mixer_elem_t *s_elem;
static long s_vol_min, s_vol_max;
static int s_mixer_percent = -1;  // the level last set or seen

// The default device's main playback control: Master when there is one (as
// PipeWire's and PulseAudio's are), else the first named one, else any.
static snd_mixer_elem_t *pick_elem(snd_mixer_t *m)
{
    static const char *const names[] = {"Master", "PCM", "Speaker", "Headphone", "Output 1", "Digital"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        for (snd_mixer_elem_t *e = snd_mixer_first_elem(m); e; e = snd_mixer_elem_next(e)) {
            if (snd_mixer_selem_is_active(e) && snd_mixer_selem_has_playback_volume(e) &&
                !strcmp(snd_mixer_selem_get_name(e), names[i])) {
                return e;
            }
        }
    }
    for (snd_mixer_elem_t *e = snd_mixer_first_elem(m); e; e = snd_mixer_elem_next(e)) {
        if (snd_mixer_selem_is_active(e) && snd_mixer_selem_has_playback_volume(e)) return e;
    }
    return NULL;
}

static bool mixer_open(void)
{
    snd_mixer_t *m;
    if (snd_mixer_open(&m, 0) < 0) return false;
    if (snd_mixer_attach(m, "default") < 0 || snd_mixer_selem_register(m, NULL, NULL) < 0 ||
        snd_mixer_load(m) < 0) {
        snd_mixer_close(m);
        return false;
    }
    snd_mixer_elem_t *e = pick_elem(m);
    if (!e || snd_mixer_selem_get_playback_volume_range(e, &s_vol_min, &s_vol_max) < 0 ||
        s_vol_max <= s_vol_min) {
        snd_mixer_close(m);
        return false;
    }
    s_mixer = m;
    s_elem = e;
    return true;
}

// The level as a percentage of the control's range; 0 while muted.
// Called with s_mixer_lock held.
static int mixer_read(void)
{
    int on = 1;
    if (snd_mixer_selem_has_playback_switch(s_elem)) {
        snd_mixer_selem_get_playback_switch(s_elem, SND_MIXER_SCHN_FRONT_LEFT, &on);
    }
    long raw = s_vol_min;
    if (snd_mixer_selem_get_playback_volume(s_elem, SND_MIXER_SCHN_FRONT_LEFT, &raw) < 0) return -1;
    if (!on) return 0;
    return (int)(((raw - s_vol_min) * 100 + (s_vol_max - s_vol_min) / 2) / (s_vol_max - s_vol_min));
}

static void mixer_write(int percent)
{
    pthread_mutex_lock(&s_mixer_lock);
    long raw = s_vol_min + ((s_vol_max - s_vol_min) * percent + 50) / 100;
    snd_mixer_selem_set_playback_volume_all(s_elem, raw);
    if (snd_mixer_selem_has_playback_switch(s_elem)) snd_mixer_selem_set_playback_switch_all(s_elem, percent > 0);
    s_mixer_percent = mixer_read();
    pthread_mutex_unlock(&s_mixer_lock);
}

// Follows changes made outside the app (another app, the OS's volume keys)
// into the settings, which the slider on Home follows.
static void *mixer_thread(void *arg)
{
    (void)arg;
    for (;;) {
        usleep(MIXER_POLL_MS * 1000);
        pthread_mutex_lock(&s_mixer_lock);
        snd_mixer_handle_events(s_mixer);
        int now = mixer_read();
        bool changed = now >= 0 && now != s_mixer_percent;
        if (changed) s_mixer_percent = now;
        pthread_mutex_unlock(&s_mixer_lock);
        if (changed && now != g_settings.volume) {
            char value[4];
            snprintf(value, sizeof(value), "%d", now);
            ESP_LOGI(TAG, "system volume now %d%%", now);
            settings_store("volume", value);
        }
    }
    return NULL;
}

// Opens the mixer and takes on the system's level as the app's own.
static void mixer_start(void)
{
    if (!mixer_open()) {
        ESP_LOGW(TAG, "no system mixer: volume is the app's own");
        return;
    }
    pthread_mutex_lock(&s_mixer_lock);
    s_mixer_percent = mixer_read();
    pthread_mutex_unlock(&s_mixer_lock);
    ESP_LOGI(TAG, "volume is the system's (%s, now %d%%)", snd_mixer_selem_get_name(s_elem), s_mixer_percent);
    if (s_mixer_percent >= 0 && s_mixer_percent != g_settings.volume) {
        char value[4];
        snprintf(value, sizeof(value), "%d", s_mixer_percent);
        settings_store("volume", value);
    }
    pthread_t t;
    if (pthread_create(&t, NULL, mixer_thread, NULL) == 0) pthread_detach(t);
}

// Sets every codec mic preamp ("ADCL PGA", "ADCR PGA" on the CardputerZero's
// ES8389) to the gain in dB, rounded down to its steps. Returns how many it set.
static int mic_pga_set(int db)
{
    int count = 0;
    for (int card = -1; snd_card_next(&card) == 0 && card >= 0;) {
        char name[16];
        snprintf(name, sizeof(name), "hw:%d", card);
        snd_mixer_t *m;
        if (snd_mixer_open(&m, 0) < 0) continue;
        if (snd_mixer_attach(m, name) < 0 || snd_mixer_selem_register(m, NULL, NULL) < 0 ||
            snd_mixer_load(m) < 0) {
            snd_mixer_close(m);
            continue;
        }
        for (snd_mixer_elem_t *e = snd_mixer_first_elem(m); e; e = snd_mixer_elem_next(e)) {
            if (!strstr(snd_mixer_selem_get_name(e), "PGA")) continue;
            long cents = (long)db * 100;
            if ((snd_mixer_selem_has_capture_volume(e) &&
                 snd_mixer_selem_set_capture_dB_all(e, cents, -1) == 0) ||
                (snd_mixer_selem_has_playback_volume(e) &&
                 snd_mixer_selem_set_playback_dB_all(e, cents, -1) == 0))
                count++;
        }
        snd_mixer_close(m);
    }
    return count;
}

#endif

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

static SDL_AudioDeviceID open_dev(bool capture, const char *name)
{
    SDL_AudioSpec want = {0}, have;
    want.freq = AUDIO_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = 512;
    SDL_AudioDeviceID id = SDL_OpenAudioDevice(name, capture, &want, &have, 0);
    if (id) SDL_PauseAudioDevice(id, 0);
    return id;
}

// The first USB microphone (a headset or USB sound card), or NULL. The system
// may keep the built-in mic as its default source even while USB audio is the
// default output, so a USB mic is chosen by name. Caller frees.
static char *usb_mic_name(void)
{
    int n = SDL_GetNumAudioDevices(1);
    for (int i = 0; i < n; i++) {
        const char *name = SDL_GetAudioDeviceName(i, 1);
        if (name && strstr(name, "USB")) return SDL_strdup(name);
    }
    return NULL;
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

// Opens a USB mic if one is plugged in, else the default source (a headset
// may come later), switching when one comes or goes, and hands on whole
// 60 ms frames.
static void *mic_thread(void *arg)
{
    (void)arg;
    int16_t frame[AUDIO_FRAME];
    size_t have = 0;
    uint32_t last_try = 0, last_check = 0;
    bool told = false, on_usb = false;
    for (;;) {
        if (!s_in) {
            uint32_t now = hal_now_ms();
            if (last_try && now - last_try < MIC_RETRY_MS) {
                usleep(100 * 1000);
                continue;
            }
            last_try = now;
            char *usb = usb_mic_name();
            s_in = usb ? open_dev(true, usb) : 0;
            on_usb = s_in != 0;
            if (!s_in) s_in = open_dev(true, NULL);
            if (!s_in) {
                if (!told) ESP_LOGW(TAG, "no microphone: %s (plug in a headset)", SDL_GetError());
                told = true;
                SDL_free(usb);
                continue;
            }
            ESP_LOGI(TAG, "microphone open: %s", on_usb ? usb : "system default");
            SDL_free(usb);
            told = false;
            have = 0;
            last_check = now;
        }
        if (SDL_GetAudioDeviceStatus(s_in) == SDL_AUDIO_STOPPED) {
            ESP_LOGW(TAG, "microphone gone");
            SDL_CloseAudioDevice(s_in);
            s_in = 0;
            continue;
        }
        if (hal_now_ms() - last_check >= MIC_RETRY_MS) {
            last_check = hal_now_ms();
            char *usb = usb_mic_name();
            bool want_usb = usb != NULL;
            SDL_free(usb);
            if (want_usb != on_usb) {
                ESP_LOGI(TAG, "USB microphone %s: switching", want_usb ? "plugged in" : "unplugged");
                SDL_CloseAudioDevice(s_in);
                s_in = 0;
                last_try = 0;
                continue;
            }
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
    s_out = open_dev(false, NULL);
    if (!s_out) {
        ESP_LOGE(TAG, "no audio output: %s", SDL_GetError());
        return ESP_FAIL;
    }
#if HAVE_ALSA_MIXER
    mixer_start();
#endif
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
#if HAVE_ALSA_MIXER
    if (s_mixer) {
        mixer_write(percent);
        return;
    }
#endif
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
#if HAVE_ALSA_MIXER
    static int logged = -1;
    int set = mic_pga_set(db);
    if (set != logged) ESP_LOGI(TAG, "mic preamp: %d dB on %d control(s)", db, set);
    logged = set;
#endif
}

void audio_set_loopback(bool on)
{
    atomic_store(&s_loopback, on);
    if (!on) audio_play_flush();
}

bool audio_loopback(void) { return atomic_load(&s_loopback); }
uint8_t audio_mic_level(void) { return (uint8_t)atomic_load(&s_mic_level); }
uint8_t audio_play_level(void) { return (uint8_t)atomic_load(&s_play_level); }
