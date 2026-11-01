#include "audio.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "board.h"

static const char *TAG = "audio";

// 2 s of receive buffering; start playback once two Zello packets (120 ms)
// have arrived, but do not strand a shorter transmission in the buffer.
#define PLAY_BUF_BYTES    (2 * AUDIO_RATE * sizeof(int16_t))
#define PLAY_START_BYTES  (2 * AUDIO_FRAME * sizeof(int16_t))
#define PLAY_START_TIMEOUT_MS 120

// Playback and capture devices. On the P4 both are the one ES8311; on the
// T-Deck the speaker amplifier and the ES7210 mic ADC are separate parts on
// separate I2S ports; on the Wio Tracker L2 the ES8311 DAC and ES7243E ADC
// share one I2S port.
static esp_codec_dev_handle_t s_out, s_in;
static StreamBufferHandle_t s_play;
static audio_mic_cb_t s_mic_cb;
static volatile bool s_playing;
static volatile bool s_loopback;
static volatile uint8_t s_mic_level, s_play_level;

#if CONFIG_IDF_TARGET_ESP32P4

static esp_err_t codec_open(void)
{
    i2s_chan_handle_t tx, rx;
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan, &tx, &rx), TAG, "i2s channel");

    i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BOARD_I2S_MCLK,
            .bclk = BOARD_I2S_BCLK,
            .ws = BOARD_I2S_WS,
            .dout = BOARD_I2S_DOUT,
            .din = BOARD_I2S_DIN,
        },
    };
    std.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx, &std), TAG, "i2s tx");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(rx, &std), TAG, "i2s rx");

    audio_codec_i2s_cfg_t i2s_cfg = {.port = I2S_NUM_0, .rx_handle = rx, .tx_handle = tx};
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = 1,
        .addr = BOARD_ES8311_ADDR << 1,  // esp_codec_dev takes the 8-bit address
        .bus_handle = board_codec_i2c_bus(),
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(data_if && ctrl_if && gpio_if, ESP_FAIL, TAG, "codec interfaces");

    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = -1,  // NS4150B is powered with the audio rail; no separate enable
        .use_mclk = true,
        .digital_mic = false,  // electret on MIC1P/MIC1N
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es_cfg);
    ESP_RETURN_ON_FALSE(codec_if, ESP_FAIL, TAG, "es8311");

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    s_out = s_in = esp_codec_dev_new(&dev_cfg);
    ESP_RETURN_ON_FALSE(s_out, ESP_FAIL, TAG, "codec dev");

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 1,
        .sample_rate = AUDIO_RATE,
        .mclk_multiple = 256,
    };
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(s_out, &fs) == ESP_CODEC_DEV_OK, ESP_FAIL, TAG, "codec open");
    return ESP_OK;
}

#elif CONFIG_ZELLO_BOARD_WIO_TRACKER_L2

// The two codecs share I2S0's clocks, so esp_codec_dev reconfigures both
// directions together; opening them with different channel counts would
// garble one. Both run stereo, and the tasks below convert to and from the
// app's mono frames.
#define AUDIO_STEREO_IO 1

// Which ES7243E channel(s) carry the microphone: 0 left, 1 right, -1 the
// average of both. Narrow this once the hardware says which one it is; the
// first seconds of capture log each channel's peak to help.
#define MIC_SLOT (-1)

// DAC digital gain at 100% volume.
#define WIO_MAX_VOL_DB 15.0

static esp_err_t codec_open(void)
{
    i2s_chan_handle_t tx, rx;
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan, &tx, &rx), TAG, "i2s channel");

    i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BOARD_I2S_MCLK,
            .bclk = BOARD_I2S_BCLK,
            .ws = BOARD_I2S_WS,
            .dout = BOARD_I2S_DOUT,
            .din = BOARD_I2S_DIN,
        },
    };
    std.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx, &std), TAG, "i2s tx");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(rx, &std), TAG, "i2s rx");

    audio_codec_i2s_cfg_t i2s_cfg = {.port = I2S_NUM_0, .rx_handle = rx, .tx_handle = tx};
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    // esp_codec_dev takes 8-bit addresses.
    audio_codec_i2c_cfg_t dac_i2c = {.port = 0, .addr = BOARD_ES8311_ADDR << 1, .bus_handle = board_codec_i2c_bus()};
    audio_codec_i2c_cfg_t adc_i2c = {.port = 0, .addr = BOARD_ES7243E_ADDR << 1, .bus_handle = board_codec_i2c_bus()};
    const audio_codec_ctrl_if_t *dac_ctrl = audio_codec_new_i2c_ctrl(&dac_i2c);
    const audio_codec_ctrl_if_t *adc_ctrl = audio_codec_new_i2c_ctrl(&adc_i2c);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(data_if && dac_ctrl && adc_ctrl && gpio_if, ESP_FAIL, TAG, "codec interfaces");

    es8311_codec_cfg_t dac_cfg = {
        .ctrl_if = dac_ctrl,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = -1,  // the amplifier is on the expander: board_audio_power()
        .use_mclk = true,
    };
    const audio_codec_if_t *dac_if = es8311_codec_new(&dac_cfg);
    ESP_RETURN_ON_FALSE(dac_if, ESP_FAIL, TAG, "es8311");
    es7243e_codec_cfg_t adc_cfg = {.ctrl_if = adc_ctrl};
    const audio_codec_if_t *adc_if = es7243e_codec_new(&adc_cfg);
    ESP_RETURN_ON_FALSE(adc_if, ESP_FAIL, TAG, "es7243e");

    esp_codec_dev_cfg_t out_cfg = {.dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = dac_if, .data_if = data_if};
    esp_codec_dev_cfg_t in_cfg = {.dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = adc_if, .data_if = data_if};
    s_out = esp_codec_dev_new(&out_cfg);
    s_in = esp_codec_dev_new(&in_cfg);
    ESP_RETURN_ON_FALSE(s_out && s_in, ESP_FAIL, TAG, "codec devs");

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 2,
        .sample_rate = AUDIO_RATE,
        .mclk_multiple = 256,
    };
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(s_out, &fs) == ESP_CODEC_DEV_OK, ESP_FAIL, TAG, "dac open");
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(s_in, &fs) == ESP_CODEC_DEV_OK, ESP_FAIL, TAG, "adc open");

    // esp_codec_dev's default curve stops at 0 dB, which is quiet through
    // this speaker. The ES8311's digital volume reaches +32 dB: let 100% use
    // some of that headroom (loud peaks clip past it).
    esp_codec_dev_vol_map_t vol_map[] = {{.vol = 0, .db_value = -50.0}, {.vol = 100, .db_value = WIO_MAX_VOL_DB}};
    esp_codec_dev_vol_curve_t curve = {.vol_map = vol_map, .count = 2};
    esp_codec_dev_set_vol_curve(s_out, &curve);
    return ESP_OK;
}

#elif CONFIG_ZELLO_BOARD_TDECK

// Which ES7210 inputs to record. The T-Deck's microphone is on one of the
// first two; recording both keeps either working, and the codec mixes them
// down to the mono stream the app reads. Narrow this once the hardware says
// which one it is.
#define MIC_SELECT (ES7210_SEL_MIC1 | ES7210_SEL_MIC2)

// Speaker: I2S0 transmit only, into the MAX98357A, which has no control
// interface. esp_codec_dev scales the volume in software.
static esp_err_t speaker_open(void)
{
    i2s_chan_handle_t tx;
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan, &tx, NULL), TAG, "speaker channel");
    i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = BOARD_SPK_BCLK,
            .ws = BOARD_SPK_WS,
            .dout = BOARD_SPK_DOUT,
            .din = I2S_GPIO_UNUSED,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx, &std), TAG, "speaker i2s");

    audio_codec_i2s_cfg_t i2s_cfg = {.port = I2S_NUM_0, .tx_handle = tx};
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    ESP_RETURN_ON_FALSE(data_if, ESP_FAIL, TAG, "speaker data if");
    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = NULL,
        .data_if = data_if,
    };
    s_out = esp_codec_dev_new(&dev_cfg);
    ESP_RETURN_ON_FALSE(s_out, ESP_FAIL, TAG, "speaker dev");

    esp_codec_dev_sample_info_t fs = {.bits_per_sample = 16, .channel = 1, .sample_rate = AUDIO_RATE};
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(s_out, &fs) == ESP_CODEC_DEV_OK, ESP_FAIL, TAG, "speaker open");
    return ESP_OK;
}

// Microphone: I2S1 receive only, from the ES7210, configured over I2C.
static esp_err_t mic_open(void)
{
    i2s_chan_handle_t rx;
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan, NULL, &rx), TAG, "mic channel");
    i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BOARD_MIC_MCLK,
            .bclk = BOARD_MIC_BCLK,
            .ws = BOARD_MIC_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = BOARD_MIC_DIN,
        },
    };
    std.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(rx, &std), TAG, "mic i2s");

    audio_codec_i2s_cfg_t i2s_cfg = {.port = I2S_NUM_1, .rx_handle = rx};
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = 0,
        .addr = BOARD_ES7210_ADDR << 1,  // esp_codec_dev takes the 8-bit address
        .bus_handle = board_codec_i2c_bus(),
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    ESP_RETURN_ON_FALSE(data_if && ctrl_if, ESP_FAIL, TAG, "mic interfaces");

    es7210_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if,
        .mic_selected = MIC_SELECT,
    };
    const audio_codec_if_t *codec_if = es7210_codec_new(&es_cfg);
    ESP_RETURN_ON_FALSE(codec_if, ESP_FAIL, TAG, "es7210");

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    s_in = esp_codec_dev_new(&dev_cfg);
    ESP_RETURN_ON_FALSE(s_in, ESP_FAIL, TAG, "mic dev");

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 1,
        .sample_rate = AUDIO_RATE,
        .mclk_multiple = 256,
    };
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(s_in, &fs) == ESP_CODEC_DEV_OK, ESP_FAIL, TAG, "mic open");
    return ESP_OK;
}

static esp_err_t codec_open(void)
{
    ESP_RETURN_ON_ERROR(speaker_open(), TAG, "speaker");
    ESP_RETURN_ON_ERROR(mic_open(), TAG, "mic");
    return ESP_OK;
}

#endif

static uint8_t peak_percent(const int16_t *pcm, size_t samples)
{
    int32_t peak = 0;
    for (size_t i = 0; i < samples; i++) {
        int32_t v = pcm[i] < 0 ? -(int32_t)pcm[i] : pcm[i];
        if (v > peak) peak = v;
    }
    return (uint8_t)(peak * 100 / 32768);
}

#ifndef AUDIO_STEREO_IO
#define AUDIO_STEREO_IO 0
#endif

#define FRAME_BYTES (AUDIO_FRAME * sizeof(int16_t))
#define IO_BYTES    (FRAME_BYTES * (AUDIO_STEREO_IO ? 2 : 1))

// Writes one mono frame, duplicated to both channels on a stereo link.
// `io` is a scratch frame of IO_BYTES (unused on a mono link).
static void out_write(int16_t *frame, int16_t *io)
{
#if AUDIO_STEREO_IO
    for (int i = 0; i < AUDIO_FRAME; i++) io[2 * i] = io[2 * i + 1] = frame[i];
    esp_codec_dev_write(s_out, io, IO_BYTES);
#else
    esp_codec_dev_write(s_out, frame, IO_BYTES);
#endif
}

// Reads one mono frame, taken from MIC_SLOT on a stereo link.
static bool in_read(int16_t *frame, int16_t *io)
{
#if AUDIO_STEREO_IO
    if (esp_codec_dev_read(s_in, io, IO_BYTES) != ESP_CODEC_DEV_OK) return false;
    // The first ~3 s: each channel's loudest sample, for finding the mic.
    static int s_frames;
    static int32_t s_peak[2];
    if (s_frames < 3000 / AUDIO_FRAME_MS) {
        for (int i = 0; i < 2 * AUDIO_FRAME; i++) {
            int32_t v = io[i] < 0 ? -(int32_t)io[i] : io[i];
            if (v > s_peak[i & 1]) s_peak[i & 1] = v;
        }
        if (++s_frames == 3000 / AUDIO_FRAME_MS) {
            ESP_LOGI(TAG, "mic peaks over 3 s: left %ld, right %ld", (long)s_peak[0], (long)s_peak[1]);
        }
    }
    for (int i = 0; i < AUDIO_FRAME; i++) {
#if MIC_SLOT >= 0
        frame[i] = io[2 * i + MIC_SLOT];
#else
        frame[i] = (int16_t)(((int32_t)io[2 * i] + io[2 * i + 1]) / 2);
#endif
    }
    return true;
#else
    return esp_codec_dev_read(s_in, frame, IO_BYTES) == ESP_CODEC_DEV_OK;
#endif
}

static void play_task(void *arg)
{
    int16_t *frame = malloc(FRAME_BYTES);
    int16_t *io = AUDIO_STEREO_IO ? malloc(IO_BYTES) : NULL;
    if (!frame || (AUDIO_STEREO_IO && !io)) vTaskDelete(NULL);
    bool waiting = false;
    TickType_t queued_since = 0;
    for (;;) {
        size_t got = 0;
        if (!s_playing) {
            size_t queued = xStreamBufferBytesAvailable(s_play);
            TickType_t now = xTaskGetTickCount();
            if (queued && !waiting) {
                queued_since = now;
                waiting = true;
            }
            if (queued >= PLAY_START_BYTES ||
                (queued && now - queued_since >= pdMS_TO_TICKS(PLAY_START_TIMEOUT_MS))) {
                s_playing = true;
            }
            if (!queued || s_playing) waiting = false;
        }
        if (s_playing) {
            got = xStreamBufferReceive(s_play, frame, FRAME_BYTES, 0);
            if (got < FRAME_BYTES) s_playing = false;  // drained: re-prebuffer
        }
        // Short read (or nothing buffered): pad the rest of the frame with
        // silence so the codec keeps its cadence. The clamp is for the
        // compiler's benefit; the stream buffer never returns more than asked.
        if (got > FRAME_BYTES) got = FRAME_BYTES;
        memset((uint8_t *)frame + got, 0, FRAME_BYTES - got);
        s_play_level = got ? peak_percent(frame, got / sizeof(int16_t)) : 0;
        // Blocks for ~60 ms; the I2S clock paces this loop.
        out_write(frame, io);
    }
}

static void mic_task(void *arg)
{
    int16_t *frame = malloc(FRAME_BYTES);
    int16_t *io = AUDIO_STEREO_IO ? malloc(IO_BYTES) : NULL;
    if (!frame || (AUDIO_STEREO_IO && !io)) vTaskDelete(NULL);
    for (;;) {
        if (!in_read(frame, io)) {
            vTaskDelay(pdMS_TO_TICKS(AUDIO_FRAME_MS));
            continue;
        }
        s_mic_level = peak_percent(frame, AUDIO_FRAME);
        if (s_loopback) {
            xStreamBufferSend(s_play, frame, FRAME_BYTES, 0);
        } else if (s_mic_cb) {
            s_mic_cb(frame, AUDIO_FRAME);
        }
    }
}

esp_err_t audio_init(audio_mic_cb_t mic_cb)
{
    s_mic_cb = mic_cb;
#if CONFIG_IDF_TARGET_ESP32S3
    // 64 KB is a quarter of the S3's heap; FreeRTOS would put it in internal RAM.
    s_play = xStreamBufferCreateWithCaps(PLAY_BUF_BYTES, 1, MALLOC_CAP_SPIRAM);
#else
    s_play = xStreamBufferCreate(PLAY_BUF_BYTES, 1);
#endif
    ESP_RETURN_ON_FALSE(s_play, ESP_ERR_NO_MEM, TAG, "play buffer");

    ESP_RETURN_ON_ERROR(board_audio_power(true), TAG, "audio power");
    ESP_RETURN_ON_ERROR(codec_open(), TAG, "codec");

    xTaskCreate(play_task, "audio_play", 4096, NULL, 20, NULL);
    xTaskCreate(mic_task, "audio_mic", 4096, NULL, 19, NULL);
    ESP_LOGI(TAG, "audio running at %d Hz mono, %d ms frames", AUDIO_RATE, AUDIO_FRAME_MS);
    return ESP_OK;
}

void audio_play_push(const int16_t *pcm, size_t samples)
{
    // Drop rather than block the decode task if we're somehow way behind.
    xStreamBufferSend(s_play, pcm, samples * sizeof(int16_t), 0);
}

void audio_play_flush(void)
{
    xStreamBufferReset(s_play);
}

bool audio_is_playing(void)
{
    return s_playing;
}

void audio_set_volume(int percent)
{
    if (s_out) esp_codec_dev_set_out_vol(s_out, percent);
}

#define BEEP_HZ    880
#define BEEP_MS    150
#define BEEP_RAMP  160      // samples of fade in and out, so it doesn't click
#define BEEP_PEAK  8000     // about -12 dBFS, near a typical voice level

void audio_beep(void)
{
    // Voice playing is left alone; an earlier beep still sounding is replaced.
    static TickType_t s_beep_end;
    if (!s_play || (s_playing && (int32_t)(xTaskGetTickCount() - s_beep_end) > 0)) return;
    enum { N = AUDIO_RATE * BEEP_MS / 1000 };
    static int16_t s_tone[N];
    if (!s_tone[1]) {
        for (int i = 0; i < N; i++) {
            float env = 1.0f;
            if (i < BEEP_RAMP) env = (float)i / BEEP_RAMP;
            if (N - i < BEEP_RAMP) env = (float)(N - i) / BEEP_RAMP;
            s_tone[i] = (int16_t)(BEEP_PEAK * env * sinf(2.0f * (float)M_PI * BEEP_HZ * i / AUDIO_RATE));
        }
    }
    // Replaces any beep still queued from a previous step.
    xStreamBufferReset(s_play);
    xStreamBufferSend(s_play, s_tone, sizeof(s_tone), 0);
    s_beep_end = xTaskGetTickCount() + pdMS_TO_TICKS(BEEP_MS + 2 * AUDIO_FRAME_MS);
}

void audio_set_mic_gain(int db)
{
    if (s_in) esp_codec_dev_set_in_gain(s_in, db);
}

void audio_set_loopback(bool on)
{
    s_loopback = on;
    if (!on) audio_play_flush();
}

bool audio_loopback(void)
{
    return s_loopback;
}

uint8_t audio_mic_level(void)
{
    return s_mic_level;
}

uint8_t audio_play_level(void)
{
    return s_play_level;
}
