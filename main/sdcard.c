#include "sdcard.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "board.h"

static const char *TAG = "sdcard";

#if CONFIG_IDF_TARGET_ESP32P4

#include "driver/sdmmc_host.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"

// Slot 0 uses the IO MUX pins 39-44 (CLK 43, CMD 44, D0-D3 39-42), whose IO
// supply is the P4's on-chip LDO channel 4, as on Espressif's P4 boards and
// in the Arduino SD_MMC setup camillia-mt uses on the T-Display-P4. The
// Tanmatsu wires its slot the same way.
#define SD_LDO_CHAN 4

static sd_pwr_ctrl_handle_t s_pwr;

static esp_err_t card_mount(sdmmc_card_t **card)
{
    esp_err_t err = board_sd_power(true);
    if (err == ESP_OK && !s_pwr) {
        sd_pwr_ctrl_ldo_config_t ldo = {.ldo_chan_id = SD_LDO_CHAN};
        err = sd_pwr_ctrl_new_on_chip_ldo(&ldo, &s_pwr);
    }
    if (err == ESP_OK) {
        sdmmc_host_t host = SDMMC_HOST_DEFAULT();
        host.slot = SDMMC_HOST_SLOT_0;
        host.pwr_ctrl_handle = s_pwr;
        sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
        slot.width = 4;
        esp_vfs_fat_sdmmc_mount_config_t mcfg = {
            .format_if_mount_failed = false,
            .max_files = 4,
            .allocation_unit_size = 16 * 1024,
        };
        err = esp_vfs_fat_sdmmc_mount(SDCARD_MOUNT, &host, &slot, &mcfg, card);
    }
    if (err != ESP_OK) board_sd_power(false);
    return err;
}

#elif CONFIG_ZELLO_BOARD_WIO_TRACKER_L2

#include "driver/sdmmc_host.h"

// SDMMC slot 1 in 1-bit mode, routed through the GPIO matrix, with the
// card's supply switched on the expander.
static esp_err_t card_mount(sdmmc_card_t **card)
{
    esp_err_t err = board_sd_power(true);
    if (err == ESP_OK) {
        sdmmc_host_t host = SDMMC_HOST_DEFAULT();
        host.slot = SDMMC_HOST_SLOT_1;
        sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
        slot.width = 1;
        slot.clk = BOARD_SD_CLK;
        slot.cmd = BOARD_SD_CMD;
        slot.d0 = BOARD_SD_D0;
        slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
        esp_vfs_fat_sdmmc_mount_config_t mcfg = {
            .format_if_mount_failed = false,
            .max_files = 4,
            .allocation_unit_size = 16 * 1024,
        };
        err = esp_vfs_fat_sdmmc_mount(SDCARD_MOUNT, &host, &slot, &mcfg, card);
    }
    if (err != ESP_OK) board_sd_power(false);
    return err;
}

#elif CONFIG_ZELLO_BOARD_TDECK

#include "driver/sdspi_host.h"

// SPI mode on the bus the LCD also uses (board.c sets it up). 20 MHz is the
// usual safe speed for a shared bus with several devices on it.
#define SD_SPI_KHZ 20000

static esp_err_t card_mount(sdmmc_card_t **card)
{
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = board_spi_host();
    host.max_freq_khz = SD_SPI_KHZ;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = board_spi_host();
    slot.gpio_cs = BOARD_SD_CS;
    esp_vfs_fat_sdmmc_mount_config_t mcfg = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };
    return esp_vfs_fat_sdspi_mount(SDCARD_MOUNT, &host, &slot, &mcfg, card);
}

#endif

static sdmmc_card_t *s_card;
static SemaphoreHandle_t s_lock;

bool sdcard_mounted(void)
{
    return s_card != NULL;
}

esp_err_t sdcard_mount(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_card) {
        // Still there? A pulled card fails this status query.
        if (sdmmc_get_status(s_card) == ESP_OK) {
            xSemaphoreGive(s_lock);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "card gone, remounting");
        esp_vfs_fat_sdcard_unmount(SDCARD_MOUNT, s_card);
        s_card = NULL;
    }

    esp_err_t err = card_mount(&s_card);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "mounted %s, %llu MB", s_card->cid.name,
                 (unsigned long long)s_card->csd.capacity * s_card->csd.sector_size / (1024 * 1024));
    } else {
        ESP_LOGW(TAG, "no card: %s", esp_err_to_name(err));
        s_card = NULL;
    }
    xSemaphoreGive(s_lock);
    return err;
}
