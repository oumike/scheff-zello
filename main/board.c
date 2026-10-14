#include "board.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "board";

#if CONFIG_IDF_TARGET_ESP32P4 || CONFIG_ZELLO_BOARD_WIO_TRACKER_L2

// PCA9535-style 16-bit I/O expander: the P4's XL9535 and the Wio Tracker L2's
// PCA9555 share the register map. Pin n is bit n (IO0..IO7, then IO10..IO17).
#define EXP_REG_IN0  0x00
#define EXP_REG_OUT0 0x02
#define EXP_REG_CFG0 0x06  // 1 = input

static i2c_master_dev_handle_t s_exp;
static SemaphoreHandle_t s_exp_lock;
static uint16_t s_out, s_cfg;  // shadow registers

static esp_err_t exp_write16(uint8_t reg, uint16_t v)
{
    uint8_t buf[3] = {reg, v & 0xff, v >> 8};
    return i2c_master_transmit(s_exp, buf, sizeof(buf), 100);
}

static esp_err_t exp_read16(uint8_t reg, uint16_t *v)
{
    uint8_t b[2];
    ESP_RETURN_ON_ERROR(i2c_master_transmit_receive(s_exp, &reg, 1, b, 2, 100), TAG, "expander read");
    *v = b[0] | (b[1] << 8);
    return ESP_OK;
}

// Adds the expander at `addr` on `bus` and loads its shadow registers.
static esp_err_t exp_init(i2c_master_bus_handle_t bus, uint16_t addr, uint32_t scl_hz)
{
    s_exp_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_exp_lock, ESP_ERR_NO_MEM, TAG, "expander lock");
    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = scl_hz,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dev, &s_exp), TAG, "expander add");
    ESP_RETURN_ON_ERROR(exp_read16(EXP_REG_OUT0, &s_out), TAG, "expander not responding");
    return exp_read16(EXP_REG_CFG0, &s_cfg);
}

// Drive an expander pin as an output at `level`. The output latch is written
// before the direction so the pin never glitches.
static esp_err_t exp_set(int pin, bool level)
{
    xSemaphoreTake(s_exp_lock, portMAX_DELAY);
    uint16_t out = level ? (s_out | (1u << pin)) : (s_out & ~(1u << pin));
    esp_err_t err = exp_write16(EXP_REG_OUT0, out);
    if (err == ESP_OK) {
        s_out = out;
        if (s_cfg & (1u << pin)) {
            uint16_t cfg = s_cfg & ~(1u << pin);
            err = exp_write16(EXP_REG_CFG0, cfg);
            if (err == ESP_OK) s_cfg = cfg;
        }
    }
    xSemaphoreGive(s_exp_lock);
    return err;
}

#endif

#if CONFIG_IDF_TARGET_ESP32S3

// Resting LiPo voltage (mV) against charge, from full down to empty.
static const struct {
    uint16_t mv;
    uint8_t percent;
} s_lipo[] = {
    {4200, 100}, {4100, 90}, {4000, 80}, {3920, 70}, {3850, 60}, {3800, 50},
    {3750, 40},  {3700, 30}, {3650, 20}, {3550, 10}, {3450, 5},  {3300, 0},
};

static uint8_t lipo_percent(int mv)
{
    const size_t n = sizeof(s_lipo) / sizeof(s_lipo[0]);
    if (mv >= s_lipo[0].mv) return 100;
    if (mv <= s_lipo[n - 1].mv) return 0;
    size_t i = 1;
    while (mv < s_lipo[i].mv) i++;
    // Linear between the two table points either side.
    int hi_mv = s_lipo[i - 1].mv, lo_mv = s_lipo[i].mv;
    int hi_p = s_lipo[i - 1].percent, lo_p = s_lipo[i].percent;
    return lo_p + (mv - lo_mv) * (hi_p - lo_p) / (hi_mv - lo_mv);
}

#endif

#if CONFIG_IDF_TARGET_ESP32P4

#define XL9535_ADDR     0x20

#define BQ27220_ADDR                    0x55
#define BQ27220_REG_STATE_OF_CHARGE     0x2c

// Expander pin numbers: IO0..IO7 = 0..7, IO10..IO17 = 8..15.
#define XIO_POWER_EN_3V3  0
#define XIO_SKY13453_VCTL 1
#define XIO_SCREEN_RST    2
#define XIO_TOUCH_RST     3
#define XIO_ETH_RST       5
#define XIO_AUDIO_PWR_EN  6
#define XIO_USB_PHY_PWR   8   // IO10
#define XIO_C6_EN         12  // IO14
#define XIO_SD_PWR_N      13  // IO15, active low

static i2c_master_bus_handle_t s_i2c1, s_i2c2;
static i2c_master_dev_handle_t s_bq27220;

static esp_err_t new_bus(int port, int sda, int scl, i2c_master_bus_handle_t *out)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = port,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&cfg, out);
}

esp_err_t board_init(void)
{
    ESP_RETURN_ON_ERROR(new_bus(0, BOARD_I2C1_SDA, BOARD_I2C1_SCL, &s_i2c1), TAG, "i2c1");
    ESP_RETURN_ON_ERROR(new_bus(1, BOARD_I2C2_SDA, BOARD_I2C2_SCL, &s_i2c2), TAG, "i2c2");

    ESP_RETURN_ON_ERROR(exp_init(s_i2c1, XL9535_ADDR, 400000), TAG, "xl9535");
    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BQ27220_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c1, &dev, &s_bq27220), TAG, "bq27220 add");

    // Same safe-state sequence as LilyGO's driver. POWER_EN_3V3 is held low
    // on V1.0 hardware; the vendor notes that changing it can lock the P4
    // into download mode.
    ESP_RETURN_ON_ERROR(exp_set(XIO_POWER_EN_3V3, 0), TAG, "3v3");
    ESP_RETURN_ON_ERROR(exp_set(XIO_SKY13453_VCTL, 1), TAG, "rf sw");
    ESP_RETURN_ON_ERROR(exp_set(XIO_SCREEN_RST, 0), TAG, "screen rst");
    ESP_RETURN_ON_ERROR(exp_set(XIO_TOUCH_RST, 0), TAG, "touch rst");
    ESP_RETURN_ON_ERROR(exp_set(XIO_ETH_RST, 0), TAG, "eth rst");
    ESP_RETURN_ON_ERROR(exp_set(XIO_C6_EN, 0), TAG, "c6 en");
    ESP_RETURN_ON_ERROR(exp_set(XIO_USB_PHY_PWR, 1), TAG, "usb phy");
    ESP_RETURN_ON_ERROR(exp_set(XIO_AUDIO_PWR_EN, 0), TAG, "audio pwr");
    ESP_RETURN_ON_ERROR(exp_set(XIO_SD_PWR_N, 1), TAG, "sd pwr");

    ESP_LOGI(TAG, "XL9535 ready (out=0x%04x cfg=0x%04x)", s_out, s_cfg);
    return ESP_OK;
}

esp_err_t board_audio_power(bool on)
{
    esp_err_t err = exp_set(XIO_AUDIO_PWR_EN, on);
    if (err == ESP_OK && on) vTaskDelay(pdMS_TO_TICKS(20));  // let AD_3V3 settle
    return err;
}

esp_err_t board_c6_enable(bool on)
{
    return exp_set(XIO_C6_EN, on);
}

esp_err_t board_sd_power(bool on)
{
    esp_err_t err = exp_set(XIO_SD_PWR_N, !on);
    if (err == ESP_OK && on) vTaskDelay(pdMS_TO_TICKS(10));
    return err;
}

esp_err_t board_screen_reset_release(bool released)
{
    return exp_set(XIO_SCREEN_RST, released);
}

esp_err_t board_touch_reset_release(bool released)
{
    return exp_set(XIO_TOUCH_RST, released);
}

esp_err_t board_battery_percent(uint8_t *percent)
{
    if (!percent) return ESP_ERR_INVALID_ARG;
    if (!s_bq27220) return ESP_ERR_INVALID_STATE;

    uint8_t reg = BQ27220_REG_STATE_OF_CHARGE;
    uint8_t data[2];
    ESP_RETURN_ON_ERROR(i2c_master_transmit_receive(s_bq27220, &reg, 1, data, sizeof(data), 100),
                        TAG, "bq27220 state of charge");
    uint16_t value = data[0] | (data[1] << 8);
    if (value > 100) return ESP_ERR_INVALID_RESPONSE;
    *percent = value;
    return ESP_OK;
}

i2c_master_bus_handle_t board_main_i2c_bus(void)
{
    return s_i2c1;
}

i2c_master_bus_handle_t board_codec_i2c_bus(void)
{
    return s_i2c2;
}

#elif CONFIG_ZELLO_BOARD_WIO_TRACKER_L2

#include "driver/gpio.h"
#include "driver/spi_master.h"

#define PCA9555_ADDR 0x21
#define I2C_HZ       100000  // what camillia-mt and Tactility run this bus at

// ADS1115: VBAT through a 2:1 divider on AIN0.
#define ADS1115_ADDR       0x48
#define ADS1115_REG_CONV   0x00
#define ADS1115_REG_CONFIG 0x01
// Start a single conversion of AIN0 against ground, +-4.096 V, 128 samples/s.
#define ADS1115_SINGLE_AIN0 0xC383
#define ADS1115_FSR_MV      4096

// Expander pins, as camillia-mt's hal/hw_wio_tracker_l2.h.
#define XIO_TOUCH_INT   3   // driven low through reset: GT911 at 0x5D
#define XIO_LCD_CS_CTRL 4   // must be high, or the audio codecs don't answer after a cold boot
#define XIO_LCD_POWER   5
#define XIO_LCD_RST     6   // active low
#define XIO_GROVE_POWER 7
#define XIO_TOUCH_RST   8   // active low
#define XIO_GNSS_RST    9   // active high
#define XIO_LED         10
#define XIO_USB_OTG     11
#define XIO_AUDIO_PA    12
#define XIO_GNSS_POWER  13
#define XIO_SD_POWER    14
#define XIO_BAT_SENSE   15

// The largest SPI transfer: one LVGL draw buffer (320 x 24 lines, RGB565).
#define SPI_MAX_TRANSFER (320 * 24 * 2)

static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_ads1115;

// Return an expander pin to an input (its output latch is left alone).
static esp_err_t exp_release(int pin)
{
    xSemaphoreTake(s_exp_lock, portMAX_DELAY);
    uint16_t cfg = s_cfg | (1u << pin);
    esp_err_t err = exp_write16(EXP_REG_CFG0, cfg);
    if (err == ESP_OK) s_cfg = cfg;
    xSemaphoreGive(s_exp_lock);
    return err;
}

esp_err_t board_init(void)
{
    gpio_config_t io = {.pin_bit_mask = 1ULL << BOARD_LORA_CS, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&io);
    gpio_set_level(BOARD_LORA_CS, 1);

    i2c_master_bus_config_t i2c = {
        .i2c_port = 0,
        .sda_io_num = BOARD_I2C_SDA,
        .scl_io_num = BOARD_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c, &s_i2c), TAG, "i2c");
    ESP_RETURN_ON_ERROR(exp_init(s_i2c, PCA9555_ADDR, I2C_HZ), TAG, "pca9555");

    // camillia-mt's power-up sequence: LCD on, everything this app doesn't
    // use (GNSS, Grove, USB OTG, LED) off, amplifier and microSD off until
    // wanted, touch held in reset with INT low to pick address 0x5D.
    ESP_RETURN_ON_ERROR(exp_set(XIO_TOUCH_INT, 0), TAG, "touch int");
    ESP_RETURN_ON_ERROR(exp_set(XIO_TOUCH_RST, 0), TAG, "touch rst");
    ESP_RETURN_ON_ERROR(exp_set(XIO_LCD_POWER, 1), TAG, "lcd power");
    ESP_RETURN_ON_ERROR(exp_set(XIO_LCD_RST, 1), TAG, "lcd rst");
    ESP_RETURN_ON_ERROR(exp_set(XIO_GROVE_POWER, 0), TAG, "grove");
    ESP_RETURN_ON_ERROR(exp_set(XIO_GNSS_POWER, 0), TAG, "gnss power");
    ESP_RETURN_ON_ERROR(exp_set(XIO_GNSS_RST, 0), TAG, "gnss rst");
    ESP_RETURN_ON_ERROR(exp_set(XIO_LED, 0), TAG, "led");
    ESP_RETURN_ON_ERROR(exp_set(XIO_USB_OTG, 0), TAG, "otg");
    ESP_RETURN_ON_ERROR(exp_set(XIO_AUDIO_PA, 0), TAG, "pa");
    ESP_RETURN_ON_ERROR(exp_set(XIO_SD_POWER, 0), TAG, "sd power");
    ESP_RETURN_ON_ERROR(exp_set(XIO_BAT_SENSE, 0), TAG, "bat sense");
    vTaskDelay(pdMS_TO_TICKS(10));

    // LCD hardware reset, then the long settle camillia-mt uses.
    ESP_RETURN_ON_ERROR(exp_set(XIO_LCD_RST, 0), TAG, "lcd rst");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(exp_set(XIO_LCD_RST, 1), TAG, "lcd rst");
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_RETURN_ON_ERROR(exp_set(XIO_LCD_CS_CTRL, 1), TAG, "lcd cs ctrl");
    vTaskDelay(pdMS_TO_TICKS(10));

    // Touch out of reset; once it has latched its address, let it drive INT.
    ESP_RETURN_ON_ERROR(exp_set(XIO_TOUCH_RST, 1), TAG, "touch rst");
    vTaskDelay(pdMS_TO_TICKS(60));
    ESP_RETURN_ON_ERROR(exp_release(XIO_TOUCH_INT), TAG, "touch int");

    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ADS1115_ADDR,
        .scl_speed_hz = I2C_HZ,
    };
    if (i2c_master_bus_add_device(s_i2c, &dev, &s_ads1115) != ESP_OK) s_ads1115 = NULL;

    // The LCD's QSPI bus. Every data line is set, as unset ones read as GPIO0
    // (the PTT button).
    spi_bus_config_t spi = {
        .sclk_io_num = BOARD_LCD_SCLK,
        .data0_io_num = BOARD_LCD_D0,
        .data1_io_num = BOARD_LCD_D1,
        .data2_io_num = BOARD_LCD_D2,
        .data3_io_num = BOARD_LCD_D3,
        .data4_io_num = -1,
        .data5_io_num = -1,
        .data6_io_num = -1,
        .data7_io_num = -1,
        .max_transfer_sz = SPI_MAX_TRANSFER,
        .flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_QUAD,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI3_HOST, &spi, SPI_DMA_CH_AUTO), TAG, "spi");

    ESP_LOGI(TAG, "PCA9555 ready (out=0x%04x cfg=0x%04x), I2C and QSPI ready", s_out, s_cfg);
    return ESP_OK;
}

spi_host_device_t board_spi_host(void)
{
    return SPI3_HOST;
}

esp_err_t board_audio_power(bool on)
{
    esp_err_t err = exp_set(XIO_AUDIO_PA, on);
    if (err == ESP_OK && on) vTaskDelay(pdMS_TO_TICKS(250));  // amplifier settle, per camillia-mt
    return err;
}

esp_err_t board_sd_power(bool on)
{
    esp_err_t err = exp_set(XIO_SD_POWER, on);
    if (err == ESP_OK && on) vTaskDelay(pdMS_TO_TICKS(10));
    return err;
}

esp_err_t board_battery_percent(uint8_t *percent)
{
    if (!percent) return ESP_ERR_INVALID_ARG;
    if (!s_ads1115) return ESP_ERR_INVALID_STATE;

    // The divider is switched in only while measuring, so it doesn't drain
    // the cell the rest of the time.
    ESP_RETURN_ON_ERROR(exp_set(XIO_BAT_SENSE, 1), TAG, "bat sense");
    vTaskDelay(pdMS_TO_TICKS(2));
    uint8_t cfg[3] = {ADS1115_REG_CONFIG, ADS1115_SINGLE_AIN0 >> 8, ADS1115_SINGLE_AIN0 & 0xff};
    esp_err_t err = i2c_master_transmit(s_ads1115, cfg, sizeof(cfg), 100);
    uint8_t reg = ADS1115_REG_CONV, raw[2] = {0};
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(10));  // one conversion at 128 samples/s is ~8 ms
        err = i2c_master_transmit_receive(s_ads1115, &reg, 1, raw, sizeof(raw), 100);
    }
    exp_set(XIO_BAT_SENSE, 0);
    ESP_RETURN_ON_ERROR(err, TAG, "ads1115");

    int16_t code = (int16_t)((raw[0] << 8) | raw[1]);
    int mv = (int)code * ADS1115_FSR_MV / 32768 * 2;  // the divider halves VBAT
    *percent = lipo_percent(mv);
    return ESP_OK;
}

i2c_master_bus_handle_t board_main_i2c_bus(void)
{
    return s_i2c;
}

i2c_master_bus_handle_t board_codec_i2c_bus(void)
{
    return s_i2c;
}

#elif CONFIG_ZELLO_BOARD_TDECK

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"

// The largest SPI transfer: one LVGL draw buffer (320 x 24 lines, RGB565).
#define SPI_MAX_TRANSFER (320 * 24 * 2)

#define BAT_ADC_CHANNEL ADC_CHANNEL_3  // GPIO4 on ADC1
#define BAT_SAMPLES     8

static i2c_master_bus_handle_t s_i2c;
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_adc_cali;

static void output_high(int pin)
{
    gpio_config_t io = {.pin_bit_mask = 1ULL << pin, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&io);
    gpio_set_level(pin, 1);
}

static void battery_init(void)
{
    adc_oneshot_unit_init_cfg_t unit = {.unit_id = ADC_UNIT_1};
    if (adc_oneshot_new_unit(&unit, &s_adc) != ESP_OK) return;
    adc_oneshot_chan_cfg_t chan = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    adc_oneshot_config_channel(s_adc, BAT_ADC_CHANNEL, &chan);
    adc_cali_curve_fitting_config_t cali = {
        .unit_id = ADC_UNIT_1,
        .chan = BAT_ADC_CHANNEL,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali, &s_adc_cali) != ESP_OK) s_adc_cali = NULL;
}

esp_err_t board_init(void)
{
    // Peripheral power first; the keyboard controller and touch need a moment
    // after it before they answer on I2C.
    output_high(BOARD_POWERON);
    // Every SPI device deselected before the bus carries anything.
    output_high(BOARD_LORA_CS);
    output_high(BOARD_SD_CS);
    output_high(BOARD_LCD_CS);
    vTaskDelay(pdMS_TO_TICKS(100));

    i2c_master_bus_config_t i2c = {
        .i2c_port = 0,
        .sda_io_num = BOARD_I2C_SDA,
        .scl_io_num = BOARD_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c, &s_i2c), TAG, "i2c");

    spi_bus_config_t spi = {
        .sclk_io_num = BOARD_SPI_SCK,
        .mosi_io_num = BOARD_SPI_MOSI,
        .miso_io_num = BOARD_SPI_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = SPI_MAX_TRANSFER,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &spi, SPI_DMA_CH_AUTO), TAG, "spi");

    battery_init();
    ESP_LOGI(TAG, "T-Deck power on, I2C and SPI ready");
    return ESP_OK;
}

spi_host_device_t board_spi_host(void)
{
    return SPI2_HOST;
}

esp_err_t board_audio_power(bool on)
{
    return ESP_OK;  // the amplifier and ADC are on the POWERON rail
}

esp_err_t board_battery_percent(uint8_t *percent)
{
    if (!percent) return ESP_ERR_INVALID_ARG;
    if (!s_adc || !s_adc_cali) return ESP_ERR_INVALID_STATE;

    int sum = 0;
    for (int i = 0; i < BAT_SAMPLES; i++) {
        int raw, mv;
        ESP_RETURN_ON_ERROR(adc_oneshot_read(s_adc, BAT_ADC_CHANNEL, &raw), TAG, "battery adc");
        ESP_RETURN_ON_ERROR(adc_cali_raw_to_voltage(s_adc_cali, raw, &mv), TAG, "battery cali");
        sum += mv;
    }
    *percent = lipo_percent(sum / BAT_SAMPLES * 2);  // the divider halves VBAT
    return ESP_OK;
}

i2c_master_bus_handle_t board_main_i2c_bus(void)
{
    return s_i2c;
}

i2c_master_bus_handle_t board_codec_i2c_bus(void)
{
    return s_i2c;
}

#endif
