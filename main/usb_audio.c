#include "usb_audio.h"

#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "usb/usb_host.h"
#include "usb/uac_host.h"
#include "bsp/power.h"
#include "audio.h"

static const char *TAG = "usb_audio";

typedef struct {
    uac_host_device_handle_t h;
    volatile bool ready;
    uint8_t channels;
    uint8_t rate_mult;  // adapter rate / AUDIO_RATE: 1 or 3
    SemaphoreHandle_t lock;  // held across read/write so a close can't race them
} stream_t;

typedef struct {
    enum { EV_CONNECT, EV_DISCONNECT } type;
    uint8_t addr, iface;
    uac_host_stream_t stream;
    uac_host_device_handle_t h;
} event_t;

static stream_t s_rx, s_tx;
static QueueHandle_t s_events;
static int8_t s_volume = 100;
static int16_t *s_scratch;  // one frame at the adapter's rate and channel count

#define MAX_MULT        3
#define SCRATCH_BYTES   (AUDIO_FRAME * MAX_MULT * 2 * sizeof(int16_t))
#define IO_TIMEOUT_MS   (2 * AUDIO_FRAME_MS)

static bool has_rate(const uac_host_dev_alt_param_t *p, uint32_t hz)
{
    if (p->sample_freq_type == 0) return hz >= p->sample_freq_lower && hz <= p->sample_freq_upper;
    for (int i = 0; i < p->sample_freq_type && i < UAC_FREQ_NUM_MAX; i++) {
        if (p->sample_freq[i] == hz) return true;
    }
    return false;
}

// Picks the 16-bit PCM alternate setting closest to what we need: 16 kHz is
// used as is, 48 kHz is converted by a factor of three.
static bool pick_format(uac_host_device_handle_t h, uint8_t alts, uac_host_stream_config_t *out, uint8_t *mult)
{
    int best = 0;
    for (uint8_t alt = 1; alt <= alts; alt++) {
        uac_host_dev_alt_param_t p;
        if (uac_host_get_device_alt_param(h, alt, &p) != ESP_OK) continue;
        if (p.format != 1 || p.bit_resolution != 16 || p.subframe_size != 2 || p.channels < 1 || p.channels > 2) {
            continue;
        }
        int score = 0;
        uint32_t hz = 0;
        if (has_rate(&p, AUDIO_RATE)) {
            score = 4, hz = AUDIO_RATE;
        } else if (has_rate(&p, AUDIO_RATE * MAX_MULT)) {
            score = 2, hz = AUDIO_RATE * MAX_MULT;
        }
        if (!score) continue;
        if (p.channels == 1) score++;  // half the bus traffic
        if (score > best) {
            best = score;
            out->channels = p.channels;
            out->bit_resolution = 16;
            out->sample_freq = hz;
            out->flags = 0;
            *mult = hz / AUDIO_RATE;
        }
    }
    return best > 0;
}

static void device_cb(uac_host_device_handle_t h, const uac_host_device_event_t ev, void *arg)
{
    switch (ev) {
    case UAC_HOST_DRIVER_EVENT_DISCONNECTED: {
        event_t e = {.type = EV_DISCONNECT, .h = h};
        xQueueSend(s_events, &e, 0);
        break;
    }
    case UAC_HOST_DEVICE_EVENT_TRANSFER_ERROR:
        ESP_LOGW(TAG, "transfer error");
        break;
    default:
        break;  // RX_DONE / TX_DONE: the reader and writer poll the buffers
    }
}

static void driver_cb(uint8_t addr, uint8_t iface, const uac_host_driver_event_t ev, void *arg)
{
    event_t e = {
        .type = EV_CONNECT,
        .addr = addr,
        .iface = iface,
        .stream = ev == UAC_HOST_DRIVER_EVENT_RX_CONNECTED ? UAC_STREAM_RX : UAC_STREAM_TX,
    };
    xQueueSend(s_events, &e, 0);
}

static void apply_volume(void)
{
    if (!s_tx.ready) return;
    xSemaphoreTake(s_tx.lock, portMAX_DELAY);
    if (s_tx.ready) {
        uac_host_device_set_mute(s_tx.h, s_volume == 0);
        if (s_volume > 0) uac_host_device_set_volume(s_tx.h, s_volume);
    }
    xSemaphoreGive(s_tx.lock);
}

static void connect(const event_t *e)
{
    stream_t *s = e->stream == UAC_STREAM_RX ? &s_rx : &s_tx;
    if (s->h) {
        ESP_LOGW(TAG, "a %s is already in use; ignoring the second one", e->stream == UAC_STREAM_RX ? "mic" : "speaker");
        return;
    }
    uac_host_device_config_t cfg = {
        .addr = e->addr,
        .iface_num = e->iface,
        .buffer_size = 4 * SCRATCH_BYTES,
        .buffer_threshold = AUDIO_FRAME * sizeof(int16_t),
        .callback = device_cb,
    };
    uac_host_device_handle_t h;
    if (uac_host_device_open(&cfg, &h) != ESP_OK) {
        ESP_LOGW(TAG, "open failed");
        return;
    }
    uac_host_dev_info_t info;
    uac_host_stream_config_t fmt;
    uint8_t mult;
    if (uac_host_get_device_info(h, &info) != ESP_OK || !pick_format(h, info.iface_alt_num, &fmt, &mult)) {
        ESP_LOGW(TAG, "no 16-bit PCM format at 16 or 48 kHz");
        uac_host_device_close(h);
        return;
    }
    if (uac_host_device_start(h, &fmt) != ESP_OK) {
        ESP_LOGW(TAG, "start failed");
        uac_host_device_close(h);
        return;
    }
    xSemaphoreTake(s->lock, portMAX_DELAY);
    s->h = h;
    s->channels = fmt.channels;
    s->rate_mult = mult;
    s->ready = true;
    xSemaphoreGive(s->lock);
    ESP_LOGI(TAG, "%s: %ls %ls, %lu Hz, %u ch", e->stream == UAC_STREAM_RX ? "microphone" : "speaker",
             info.iManufacturer, info.iProduct, (unsigned long)fmt.sample_freq, fmt.channels);
    if (s == &s_tx) apply_volume();
}

static void disconnect(uac_host_device_handle_t h)
{
    stream_t *s = h == s_rx.h ? &s_rx : h == s_tx.h ? &s_tx : NULL;
    if (!s) return;
    s->ready = false;
    xSemaphoreTake(s->lock, portMAX_DELAY);  // waits out a read or write in flight
    s->h = NULL;
    xSemaphoreGive(s->lock);
    uac_host_device_close(h);
    ESP_LOGI(TAG, "%s unplugged", s == &s_rx ? "microphone" : "speaker");
}

static void manager_task(void *arg)
{
    event_t e;
    for (;;) {
        if (!xQueueReceive(s_events, &e, portMAX_DELAY)) continue;
        if (e.type == EV_CONNECT) connect(&e);
        else disconnect(e.h);
    }
}

static void host_task(void *arg)
{
    for (;;) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) ESP_LOGD(TAG, "all devices gone");
    }
}

esp_err_t usb_audio_init(void)
{
    s_scratch = heap_caps_malloc(SCRATCH_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_events = xQueueCreate(8, sizeof(event_t));
    s_rx.lock = xSemaphoreCreateMutex();
    s_tx.lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_scratch && s_events && s_rx.lock && s_tx.lock, ESP_ERR_NO_MEM, TAG, "alloc");

    // 5 V for the USB-A port comes from the PMIC through the coprocessor.
    ESP_RETURN_ON_ERROR(bsp_power_set_usb_host_boost_enabled(true), TAG, "host boost");

    const usb_host_config_t host = {.skip_phy_setup = false, .intr_flags = ESP_INTR_FLAG_LOWMED};
    ESP_RETURN_ON_ERROR(usb_host_install(&host), TAG, "usb host");
    xTaskCreatePinnedToCore(host_task, "usb_host", 4096, NULL, 2, NULL, 0);

    const uac_host_driver_config_t drv = {
        .create_background_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .core_id = 0,
        .callback = driver_cb,
    };
    ESP_RETURN_ON_ERROR(uac_host_install(&drv), TAG, "uac host");
    xTaskCreate(manager_task, "usb_audio", 4096, NULL, 4, NULL);
    ESP_LOGI(TAG, "waiting for a USB audio adapter");
    return ESP_OK;
}

bool usb_audio_mic_ready(void)
{
    return s_rx.ready;
}

bool usb_audio_speaker_ready(void)
{
    return s_tx.ready;
}

bool usb_audio_read(int16_t *frame)
{
    if (!s_rx.ready) return false;
    bool ok = false;
    xSemaphoreTake(s_rx.lock, portMAX_DELAY);
    if (s_rx.ready) {
        const uint32_t per = s_rx.rate_mult * s_rx.channels;
        const uint32_t want = AUDIO_FRAME * per * sizeof(int16_t);
        uint32_t have = 0;
        TickType_t end = xTaskGetTickCount() + pdMS_TO_TICKS(IO_TIMEOUT_MS);
        while (have < want && (int32_t)(end - xTaskGetTickCount()) > 0) {
            uint32_t got = 0;
            if (uac_host_device_read(s_rx.h, (uint8_t *)s_scratch + have, want - have, &got, 20) == ESP_OK) have += got;
        }
        if (have == want) {
            // Average each group of channels and rate-converted samples.
            for (int i = 0; i < AUDIO_FRAME; i++) {
                int32_t sum = 0;
                for (uint32_t k = 0; k < per; k++) sum += s_scratch[i * per + k];
                frame[i] = (int16_t)(sum / (int32_t)per);
            }
            ok = true;
        }
    }
    xSemaphoreGive(s_rx.lock);
    return ok;
}

bool usb_audio_write(const int16_t *frame)
{
    if (!s_tx.ready) return false;
    bool ok = false;
    xSemaphoreTake(s_tx.lock, portMAX_DELAY);
    if (s_tx.ready) {
        static int16_t s_prev;
        const int mult = s_tx.rate_mult;
        int n = 0;
        // Linear interpolation up to the adapter's rate, then to its channels.
        for (int i = 0; i < AUDIO_FRAME; i++) {
            int32_t a = i ? frame[i - 1] : s_prev, b = frame[i];
            for (int k = 1; k <= mult; k++) {
                int16_t v = (int16_t)(a + (b - a) * k / mult);
                for (int c = 0; c < s_tx.channels; c++) s_scratch[n++] = v;
            }
        }
        s_prev = frame[AUDIO_FRAME - 1];
        ok = uac_host_device_write(s_tx.h, (uint8_t *)s_scratch, n * sizeof(int16_t), IO_TIMEOUT_MS) == ESP_OK;
    }
    xSemaphoreGive(s_tx.lock);
    return ok;
}

void usb_audio_set_volume(int percent)
{
    s_volume = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    apply_volume();
}
