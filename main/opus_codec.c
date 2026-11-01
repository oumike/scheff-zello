#include "opus_codec.h"

#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/base64.h"
#include "opus.h"
#include "audio.h"

static const char *TAG = "opus";

// Voice at 16 kHz: 24 kbit/s is transparent enough for speech and small enough
// that a 60 ms packet still fits comfortably in one WebSocket frame.
#define ENCODE_BITRATE  24000
#define ENCODE_COMPLEXITY 5

// A decoded packet can be longer than ours if the sender used a bigger frame;
// 120 ms at AUDIO_RATE is the most Opus will ever hand back.
#define DECODE_MAX_SAMPLES (AUDIO_RATE / 1000 * 120)

static OpusEncoder *s_enc;
static OpusDecoder *s_dec;
static SemaphoreHandle_t s_decoder_lock;
static char s_header[12];

esp_err_t opus_codec_init(void)
{
    s_decoder_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_decoder_lock, ESP_ERR_NO_MEM, TAG, "decoder lock");
    int err;
    s_enc = opus_encoder_create(AUDIO_RATE, 1, OPUS_APPLICATION_VOIP, &err);
    ESP_RETURN_ON_FALSE(s_enc && err == OPUS_OK, ESP_FAIL, TAG, "encoder: %s", opus_strerror(err));
    opus_encoder_ctl(s_enc, OPUS_SET_BITRATE(ENCODE_BITRATE));
    opus_encoder_ctl(s_enc, OPUS_SET_COMPLEXITY(ENCODE_COMPLEXITY));
    opus_encoder_ctl(s_enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    // Constant bitrate: Zello's apps interoperate with it, and it keeps the
    // packet rate steady, which matters more than a few saved bits here.
    opus_encoder_ctl(s_enc, OPUS_SET_VBR(0));
    // Half duplex over Wi-Fi: in-band FEC and a 5% loss hint cost a little
    // bitrate and recover single dropped packets.
    opus_encoder_ctl(s_enc, OPUS_SET_INBAND_FEC(1));
    opus_encoder_ctl(s_enc, OPUS_SET_PACKET_LOSS_PERC(5));

    s_dec = opus_decoder_create(AUDIO_RATE, 1, &err);
    ESP_RETURN_ON_FALSE(s_dec && err == OPUS_OK, ESP_FAIL, TAG, "decoder: %s", opus_strerror(err));

    const uint8_t header[4] = {AUDIO_RATE & 0xff, (AUDIO_RATE >> 8) & 0xff, 1, AUDIO_FRAME_MS};
    size_t written = 0;
    ESP_RETURN_ON_FALSE(mbedtls_base64_encode((unsigned char *)s_header, sizeof(s_header), &written, header,
                                              sizeof(header)) == 0,
                        ESP_FAIL, TAG, "codec header");
    s_header[written] = 0;
    ESP_LOGI(TAG, "%d Hz mono, %d ms packets, %d bit/s (header %s)", AUDIO_RATE, AUDIO_FRAME_MS, ENCODE_BITRATE,
             s_header);
    return ESP_OK;
}

int opus_codec_encode(const int16_t *pcm, uint8_t *out, size_t out_size)
{
    if (!s_enc) return -1;
    return opus_encode(s_enc, pcm, AUDIO_FRAME, out, out_size);
}

int opus_codec_decode(const uint8_t *packet, size_t len, int16_t *pcm, size_t max_samples)
{
    if (!s_dec) return -1;
    if (max_samples > DECODE_MAX_SAMPLES) max_samples = DECODE_MAX_SAMPLES;
    xSemaphoreTake(s_decoder_lock, portMAX_DELAY);
    int samples = opus_decode(s_dec, packet, len, pcm, max_samples, 0);
    xSemaphoreGive(s_decoder_lock);
    return samples;
}

void opus_codec_reset_decoder(void)
{
    if (!s_dec) return;
    xSemaphoreTake(s_decoder_lock, portMAX_DELAY);
    opus_decoder_ctl(s_dec, OPUS_RESET_STATE);
    xSemaphoreGive(s_decoder_lock);
}

const char *opus_codec_header_base64(void)
{
    return s_header;
}
