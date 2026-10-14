#include "display.h"

#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_lvgl_port.h"
#include "board.h"

static const char *TAG = "display";

#if CONFIG_IDF_TARGET_ESP32P4

#include "esp_lcd_mipi_dsi.h"
#include "esp_ldo_regulator.h"
#include "hal/axi_icm_ll.h"
#include "hal/dw_gdma_ll.h"

// Panel timing and init sequence follow camillia-mt's T-Display P4 port
// (src/hal/hw_tdisplay_p4.h, tdisplay_p4_display.h), which in turn follows
// LilyGO's ECO2 RM69A10 example: two lanes at 1 Gbps. Their 80 MHz pixel
// clock (~66 fps) is lowered to 48 MHz (~40 fps): scan-out reads the whole
// frame buffer from PSRAM every frame, and at 80 MHz it sometimes couldn't
// keep up, which flashed the panel blue. This UI has no use for 66 fps.
#define DSI_LANES          2
#define DSI_LANE_MBPS      1000
#define DSI_PHY_LDO_CHAN   3
#define DSI_PHY_LDO_MV     2500
#define DPI_CLOCK_MHZ      48
#define DPI_HSYNC          50
#define DPI_HBP            150
#define DPI_HFP            50
#define DPI_VSYNC          40
#define DPI_VBP            120
#define DPI_VFP            80

#define BRIGHTNESS_DEFAULT 160
#define DRAW_BUF_LINES     12  // internal RAM: TLS needs what is left

// GT9895 on I2C bus 1. It reports in its own 1060x2400 space, scaled here.
#define GT9895_ADDR         0x5D
#define GT9895_RAW_W        1060
#define GT9895_RAW_H        2400
#define GT9895_FW_VER_ADDR  0x00010014
#define GT9895_RUNTIME_ADDR 0x00010070
#define GT9895_FW_INFO_LEN  28
#define GT9895_RT_MIN_LEN   64
#define GT9895_RT_MAX_LEN   1024
#define GT9895_HDR_LEN      8
#define GT9895_CONTACT_LEN  8
#define GT9895_CSUM_LEN     2
#define GT9895_MAX_CONTACTS 10
#define GT9895_TOUCH_EVENT  0x80

typedef struct {
    uint8_t cmd;
    uint8_t len;
    uint8_t data[4];
} init_cmd_t;

static const init_cmd_t s_init_cmds[] = {
    {0xFE, 1, {0xFD}},
    {0x80, 1, {0xFC}},
    {0xFE, 1, {0x00}},
    {0x2A, 4, {0x00, 0x00, 0x02, 0x37}},  // columns 0..567
    {0x2B, 4, {0x00, 0x00, 0x04, 0xCF}},  // rows 0..1231
    {0x31, 4, {0x00, 0x03, 0x02, 0x34}},
    {0x30, 4, {0x00, 0x00, 0x04, 0xCF}},
    {0x12, 1, {0x00}},
    {0x35, 1, {0x00}},                    // tearing effect on
    {0x51, 1, {0x00}},                    // brightness 0 until ready
    {0x11, 0, {0}},                       // sleep out
};

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static i2c_master_dev_handle_t s_touch;
static uint32_t s_touch_report_addr;

// ---- GT9895 ----------------------------------------------------------------

static uint16_t le16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

static bool gt_checksum_ok(const uint8_t *d, size_t len)
{
    if (len < GT9895_CSUM_LEN) return false;
    uint32_t sum = 0;
    for (size_t i = 0; i < len - GT9895_CSUM_LEN; i++) sum += d[i];
    return (uint16_t)sum == le16(d + len - GT9895_CSUM_LEN);
}

static esp_err_t gt_read(uint32_t addr, uint8_t *buf, size_t len)
{
    uint8_t a[4] = {addr >> 24, addr >> 16, addr >> 8, addr};
    return i2c_master_transmit_receive(s_touch, a, sizeof(a), buf, len, 50);
}

static esp_err_t gt_write(uint32_t addr, const uint8_t *buf, size_t len)
{
    uint8_t b[4 + 4] = {addr >> 24, addr >> 16, addr >> 8, addr};
    if (len > 4) return ESP_ERR_INVALID_SIZE;
    memcpy(b + 4, buf, len);
    return i2c_master_transmit(s_touch, b, 4 + len, 50);
}

static void gt_clear_status(void)
{
    const uint8_t zero = 0;
    gt_write(s_touch_report_addr, &zero, 1);
}

// Finds the touch report address in the runtime info table, as LilyGO's and
// camillia-mt's drivers do: skip the header, then five length-prefixed arrays.
static esp_err_t gt9895_init(void)
{
    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = GT9895_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(board_main_i2c_bus(), &dev, &s_touch), TAG, "touch add");

    ESP_RETURN_ON_ERROR(board_touch_reset_release(false), TAG, "touch rst");
    vTaskDelay(pdMS_TO_TICKS(30));
    ESP_RETURN_ON_ERROR(board_touch_reset_release(true), TAG, "touch rst");
    vTaskDelay(pdMS_TO_TICKS(100));

    uint8_t fw[GT9895_FW_INFO_LEN];
    ESP_RETURN_ON_ERROR(gt_read(GT9895_FW_VER_ADDR, fw, sizeof(fw)), TAG, "GT9895 not responding");
    ESP_RETURN_ON_FALSE(gt_checksum_ok(fw, sizeof(fw)) && !memcmp(fw + 10, "9895", 4),
                        ESP_ERR_INVALID_RESPONSE, TAG, "GT9895 firmware probe failed");

    uint8_t lb[2];
    ESP_RETURN_ON_ERROR(gt_read(GT9895_RUNTIME_ADDR, lb, sizeof(lb)), TAG, "runtime len");
    size_t len = le16(lb);
    ESP_RETURN_ON_FALSE(len >= GT9895_RT_MIN_LEN && len <= GT9895_RT_MAX_LEN,
                        ESP_ERR_INVALID_RESPONSE, TAG, "bad runtime len %u", (unsigned)len);

    uint8_t *rt = malloc(len);
    ESP_RETURN_ON_FALSE(rt, ESP_ERR_NO_MEM, TAG, "runtime buf");
    esp_err_t err = gt_read(GT9895_RUNTIME_ADDR, rt, len);
    if (err == ESP_OK && !gt_checksum_ok(rt, len)) err = ESP_ERR_INVALID_CRC;

    size_t off = 2 + 16 + 10 + 4;
    for (int i = 0; err == ESP_OK && i < 5; i++) {
        if (off >= len - 2) { err = ESP_ERR_INVALID_SIZE; break; }
        size_t n = (size_t)rt[off++] * 2;
        if (n > len - 2 - off) { err = ESP_ERR_INVALID_SIZE; break; }
        off += n;
    }
    if (err == ESP_OK && off + 48 > len - 2) err = ESP_ERR_INVALID_SIZE;
    if (err == ESP_OK) s_touch_report_addr = le32(rt + off + 44);
    free(rt);
    ESP_RETURN_ON_ERROR(err, TAG, "runtime table");
    ESP_RETURN_ON_FALSE(s_touch_report_addr, ESP_ERR_INVALID_RESPONSE, TAG, "no touch report address");

    ESP_LOGI(TAG, "GT9895 ready, report at 0x%08lx", (unsigned long)s_touch_report_addr);
    return ESP_OK;
}

// The GT9895 posts a report once per scan and we clear it after reading, so
// a poll between scans finds no new report. That means "unchanged", not
// "released": treating it as a release made a held finger look like a string
// of taps (so a long press became a click). A release is the controller's
// own report with no contacts; if reports stop altogether while pressed (a
// missed release), let go after RELEASE_TIMEOUT_US.
#define RELEASE_TIMEOUT_US (300 * 1000)

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    static int32_t last_x, last_y;
    static bool pressed;
    static int64_t last_report_us;

    data->point.x = last_x;
    data->point.y = last_y;

    uint8_t hdr[GT9895_HDR_LEN];
    bool fresh = gt_read(s_touch_report_addr, hdr, sizeof(hdr)) == ESP_OK && hdr[0] != 0;
    if (!fresh) {
        if (pressed && esp_timer_get_time() - last_report_us > RELEASE_TIMEOUT_US) pressed = false;
        data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
        return;
    }

    uint8_t count = hdr[2] & 0x0F;
    if (!gt_checksum_ok(hdr, sizeof(hdr)) || count > GT9895_MAX_CONTACTS) {
        gt_clear_status();  // corrupt report: ignore it, keep the last state
    } else if (!(hdr[0] & GT9895_TOUCH_EVENT) || count == 0) {
        gt_clear_status();
        pressed = false;  // the controller says the finger lifted
        last_report_us = esp_timer_get_time();
    } else {
        // Only the first contact is used: LVGL here is single-pointer.
        uint8_t pl[GT9895_MAX_CONTACTS * GT9895_CONTACT_LEN + GT9895_CSUM_LEN];
        size_t pl_len = count * GT9895_CONTACT_LEN + GT9895_CSUM_LEN;
        if (gt_read(s_touch_report_addr + GT9895_HDR_LEN, pl, pl_len) == ESP_OK && gt_checksum_ok(pl, pl_len)) {
            int32_t x = (int32_t)le16(pl + 2) * DISPLAY_H_RES / GT9895_RAW_W;
            int32_t y = (int32_t)le16(pl + 4) * DISPLAY_V_RES / GT9895_RAW_H;
            last_x = x < DISPLAY_H_RES ? x : DISPLAY_H_RES - 1;
            last_y = y < DISPLAY_V_RES ? y : DISPLAY_V_RES - 1;
            pressed = true;
            last_report_us = esp_timer_get_time();
        }
        gt_clear_status();
    }
    data->point.x = last_x;
    data->point.y = last_y;
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

// ---- RM69A10 ---------------------------------------------------------------

static esp_err_t panel_init(void)
{
    esp_ldo_channel_handle_t ldo;
    esp_ldo_channel_config_t ldo_cfg = {.chan_id = DSI_PHY_LDO_CHAN, .voltage_mv = DSI_PHY_LDO_MV};
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_cfg, &ldo), TAG, "dsi phy ldo");

    ESP_RETURN_ON_ERROR(board_screen_reset_release(false), TAG, "screen rst");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(board_screen_reset_release(true), TAG, "screen rst");
    vTaskDelay(pdMS_TO_TICKS(120));

    esp_lcd_dsi_bus_handle_t bus;
    esp_lcd_dsi_bus_config_t bus_cfg = {
        .bus_id = 0,
        .num_data_lanes = DSI_LANES,
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = DSI_LANE_MBPS,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_cfg, &bus), TAG, "dsi bus");

    esp_lcd_dbi_io_config_t io_cfg = {.virtual_channel = 0, .lcd_cmd_bits = 8, .lcd_param_bits = 8};
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(bus, &io_cfg, &s_io), TAG, "dbi io");

    esp_lcd_dpi_panel_config_t dpi_cfg = {
        .virtual_channel = 0,
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = DPI_CLOCK_MHZ,
        .pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565,
        // One frame buffer, scanned out and drawn into but never swapped, as
        // camillia-mt's LovyanGFX port does. Swapping two PSRAM frame buffers
        // (LVGL direct mode) flashed the panel blue now and then.
        .num_fbs = 1,
        .video_timing = {
            .h_size = DISPLAY_H_RES,
            .v_size = DISPLAY_V_RES,
            .hsync_pulse_width = DPI_HSYNC,
            .hsync_back_porch = DPI_HBP,
            .hsync_front_porch = DPI_HFP,
            .vsync_pulse_width = DPI_VSYNC,
            .vsync_back_porch = DPI_VBP,
            .vsync_front_porch = DPI_VFP,
        },
        .flags.use_dma2d = true,
    };
    // The scan-out reads the frame buffer from PSRAM through DW-GDMA's memory
    // port, contending with the CPU cache, DMA2D and SDIO on the AXI bus. When
    // it loses, the DSI bridge's FIFO underruns and the panel flashes blue
    // (esp_lcd logs "underrun happens"). Give its reads top priority.
    axi_icm_ll_set_dw_gdma_qos_arbiter_prio(DW_GDMA_LL_MASTER_PORT_MEMORY, 0, 15);
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_dpi(bus, &dpi_cfg, &s_panel), TAG, "dpi panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "dpi init");

    // Same order as LovyanGFX's Panel_DSI: video stream first, then the
    // vendor sequence over the command channel.
    for (size_t i = 0; i < sizeof(s_init_cmds) / sizeof(s_init_cmds[0]); i++) {
        const init_cmd_t *c = &s_init_cmds[i];
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(s_io, c->cmd, c->len ? c->data : NULL, c->len),
                            TAG, "init cmd 0x%02x", c->cmd);
    }
    vTaskDelay(pdMS_TO_TICKS(120));
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(s_io, 0x29, NULL, 0), TAG, "display on");
    return ESP_OK;
}

void display_set_brightness(uint8_t level)
{
    if (s_io) esp_lcd_panel_io_tx_param(s_io, 0x51, &level, 1);
}

esp_err_t display_init(void)
{
    ESP_RETURN_ON_ERROR(panel_init(), TAG, "panel");

    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_stack = 8192;
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "lvgl port");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = s_io,
        .panel_handle = s_panel,
        // Partial rendering into small internal buffers; the DPI driver copies
        // each finished area into the frame buffer with DMA2D.
        .buffer_size = DISPLAY_H_RES * DRAW_BUF_LINES,
        .double_buffer = true,
        .hres = DISPLAY_H_RES,
        .vres = DISPLAY_V_RES,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = true,
        },
    };
    const lvgl_port_display_dsi_cfg_t dsi_cfg = {
        .flags.avoid_tearing = false,
    };
    lv_display_t *disp = lvgl_port_add_disp_dsi(&disp_cfg, &dsi_cfg);
    ESP_RETURN_ON_FALSE(disp, ESP_FAIL, TAG, "lvgl display");

    // Touch is optional: the UI still draws without it, and the serial
    // console remains a way in.
    esp_err_t terr = gt9895_init();
    if (terr == ESP_OK) {
        lvgl_port_lock(0);
        lv_indev_t *indev = lv_indev_create();
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(indev, touch_read_cb);
        lv_indev_set_display(indev, disp);
        // ~330 ppi: LVGL's default 10 px drag threshold is under a millimetre,
        // so a steady hold could turn into a scroll and cancel the press.
        lv_indev_set_scroll_limit(indev, 24);
        lvgl_port_unlock();
    } else {
        ESP_LOGE(TAG, "touch unavailable: %s", esp_err_to_name(terr));
    }

    // The panel stays dark until display_reveal(): until the UI has drawn,
    // the frame buffers hold nothing worth showing.
    ESP_LOGI(TAG, "%dx%d AMOLED ready, %u bytes internal RAM free", DISPLAY_H_RES, DISPLAY_V_RES,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return ESP_OK;
}

static bool s_test_pattern;

esp_err_t display_set_test_pattern(bool on)
{
    if (!s_panel) return ESP_ERR_INVALID_STATE;
    esp_err_t err = esp_lcd_dpi_panel_set_pattern(s_panel, on ? MIPI_DSI_PATTERN_BAR_HORIZONTAL : MIPI_DSI_PATTERN_NONE);
    if (err == ESP_OK) s_test_pattern = on;
    return err;
}

bool display_test_pattern(void)
{
    return s_test_pattern;
}

#elif CONFIG_ZELLO_BOARD_WIO_TRACKER_L2

#include "esp_lcd_touch_gt911.h"
#include "lcd_nv3031b.h"

// The NV3031B's native 240x320 portrait turned to landscape: row/column
// exchange plus a mirrored y (MADCTL 0xA0), as camillia-mt and Tactility set
// it. 40 MHz QSPI as Tactility and Launcher run it.
#define LCD_SPI_HZ         (40 * 1000 * 1000)
#define DRAW_BUF_LINES     24
#define BRIGHTNESS_DEFAULT 180

// LP5814 LED driver: the backlight is its four channels in parallel.
#define LP5814_PWM_REG 0x18  // 0x18..0x1B, one per channel
#define LP5814_CHANNELS 4

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static i2c_master_dev_handle_t s_lp5814;

static esp_err_t lp5814_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_lp5814, buf, sizeof(buf), 100);
}

void display_set_brightness(uint8_t level)
{
    if (!s_lp5814) return;
    for (int i = 0; i < LP5814_CHANNELS; i++) lp5814_write(LP5814_PWM_REG + i, level);
}

// Brought up before the touch controller is probed, as camillia-mt does.
static esp_err_t backlight_init(void)
{
    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BOARD_LP5814_ADDR,
        .scl_speed_hz = 100000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(board_main_i2c_bus(), &dev, &s_lp5814), TAG, "lp5814 add");

    // camillia-mt's sequence: chip enable, manual PWM mode on all four
    // channels at a fixed drive current, then latch the configuration.
    static const uint8_t seq[][2] = {
        {0x00, 0x01}, {0x01, 0x01}, {0x02, 0x00}, {0x04, 0x4E}, {0x05, 0xF0},
        {0x14, 200},  {0x15, 200},  {0x16, 200},  {0x17, 200},
        {0x18, 0},    {0x19, 0},    {0x1A, 0},    {0x1B, 0},  // dark until display_reveal()
        {0x02, 0x0F}, {0x0F, 0x55},
    };
    for (size_t i = 0; i < sizeof(seq) / sizeof(seq[0]); i++) {
        esp_err_t err = lp5814_write(seq[i][0], seq[i][1]);
        if (err != ESP_OK) {
            i2c_master_bus_rm_device(s_lp5814);
            s_lp5814 = NULL;
            return err;
        }
    }
    return ESP_OK;
}

static esp_err_t panel_init(void)
{
    // QSPI framing: 32-bit commands (opcode + command), no DC line.
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = BOARD_LCD_CS,
        .dc_gpio_num = -1,
        .spi_mode = 3,
        .pclk_hz = LCD_SPI_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 32,
        .lcd_param_bits = 8,
        .flags.quad_mode = 1,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)board_spi_host(), &io_cfg, &s_io),
                        TAG, "panel io");

    esp_lcd_panel_dev_config_t dev_cfg = {
        .reset_gpio_num = -1,  // board_init() already reset it through the expander
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(lcd_new_panel_nv3031b(s_io, &dev_cfg, &s_panel), TAG, "nv3031b");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "init");
    // Unlike the T-Deck's ST7789, this panel shows true colours uninverted.
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, false), TAG, "invert");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(s_panel, true), TAG, "swap");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel, false, true), TAG, "mirror");
    return esp_lcd_panel_disp_on_off(s_panel, true);
}

// GT911 reports in the panel's portrait 240x320 space; landscape is x = 319 -
// raw y, y = raw x (Tactility's device tree: swap_xy and mirror_y). board_init()
// strapped it to 0x5D through its reset.
static esp_err_t touch_init(lv_display_t *disp)
{
    uint16_t addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS;
    if (i2c_master_probe(board_main_i2c_bus(), addr, 50) != ESP_OK) {
        addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP;
        ESP_RETURN_ON_ERROR(i2c_master_probe(board_main_i2c_bus(), addr, 50), TAG, "no GT911 at 0x5D or 0x14");
    }

    esp_lcd_panel_io_handle_t tio;
    esp_lcd_panel_io_i2c_config_t tio_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    tio_cfg.dev_addr = addr;
    tio_cfg.scl_speed_hz = 100000;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(board_main_i2c_bus(), &tio_cfg, &tio), TAG, "touch io");

    esp_lcd_touch_config_t tp_cfg = {
        .x_max = DISPLAY_V_RES,  // portrait width
        .y_max = DISPLAY_H_RES,  // portrait height
        .rst_gpio_num = -1,      // on the expander; board_init() handled it
        .int_gpio_num = GPIO_NUM_NC,  // also on the expander: polled
        .flags = {
            .swap_xy = 1,
            .mirror_x = 0,
            .mirror_y = 1,
        },
    };
    esp_lcd_touch_handle_t tp;
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_gt911(tio, &tp_cfg, &tp), TAG, "gt911");

    const lvgl_port_touch_cfg_t touch_cfg = {.disp = disp, .handle = tp};
    ESP_RETURN_ON_FALSE(lvgl_port_add_touch(&touch_cfg), ESP_FAIL, TAG, "lvgl touch");
    ESP_LOGI(TAG, "GT911 ready at 0x%02x", addr);
    return ESP_OK;
}

esp_err_t display_init(void)
{
    // Without the backlight the screen is dark, but touch and the UI still work.
    esp_err_t bl_err = backlight_init();
    if (bl_err != ESP_OK) ESP_LOGE(TAG, "backlight unavailable: %s", esp_err_to_name(bl_err));
    ESP_RETURN_ON_ERROR(panel_init(), TAG, "panel");

    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_stack = 8192;
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "lvgl port");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = s_io,
        .panel_handle = s_panel,
        .buffer_size = DISPLAY_H_RES * DRAW_BUF_LINES,
        .double_buffer = true,
        .hres = DISPLAY_H_RES,
        .vres = DISPLAY_V_RES,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = {
            .swap_xy = true,
            .mirror_x = false,
            .mirror_y = true,
        },
        .flags = {
            .buff_dma = true,
            .swap_bytes = true,  // the NV3031B takes RGB565 big-endian
        },
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    ESP_RETURN_ON_FALSE(disp, ESP_FAIL, TAG, "lvgl display");

    // Touch is the only way to drive this UI, but carry on without it so the
    // console and web config still work.
    esp_err_t terr = touch_init(disp);
    if (terr != ESP_OK) ESP_LOGE(TAG, "touch unavailable: %s", esp_err_to_name(terr));

    ESP_LOGI(TAG, "%dx%d LCD ready, %u bytes internal RAM free", DISPLAY_H_RES, DISPLAY_V_RES,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return ESP_OK;
}

esp_err_t display_set_test_pattern(bool on)
{
    return ESP_ERR_NOT_SUPPORTED;
}

bool display_test_pattern(void)
{
    return false;
}

#elif CONFIG_ZELLO_BOARD_TDECK

#include "driver/ledc.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_touch_gt911.h"

// Timing and orientation follow camillia-mt's T-Deck port: 40 MHz writes, and
// the panel's native 240x320 portrait turned to landscape (LovyanGFX rotation
// 1, i.e. row/column exchange plus a mirrored x).
#define LCD_SPI_HZ         (40 * 1000 * 1000)
#define DRAW_BUF_LINES     24
#define BRIGHTNESS_DEFAULT 180

#define BL_TIMER    LEDC_TIMER_0
#define BL_CHANNEL  LEDC_CHANNEL_0
#define BL_BITS     LEDC_TIMER_13_BIT
#define BL_MAX_DUTY ((1 << 13) - 1)

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;

static esp_err_t backlight_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = BL_BITS,
        .timer_num = BL_TIMER,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "backlight timer");
    ledc_channel_config_t chan = {
        .gpio_num = BOARD_LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = BL_CHANNEL,
        .timer_sel = BL_TIMER,
        .duty = 0,  // dark until display_reveal()
    };
    return ledc_channel_config(&chan);
}

void display_set_brightness(uint8_t level)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BL_CHANNEL, (uint32_t)level * BL_MAX_DUTY / 255);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BL_CHANNEL);
}

static esp_err_t panel_init(void)
{
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = BOARD_LCD_CS,
        .dc_gpio_num = BOARD_LCD_DC,
        .spi_mode = 0,
        .pclk_hz = LCD_SPI_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)board_spi_host(), &io_cfg, &s_io),
                        TAG, "panel io");

    esp_lcd_panel_dev_config_t dev_cfg = {
        .reset_gpio_num = -1,  // no reset line; esp_lcd_panel_reset() sends a software reset
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(s_io, &dev_cfg, &s_panel), TAG, "st7789");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, true), TAG, "invert");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(s_panel, true), TAG, "swap");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel, true, false), TAG, "mirror");
    return esp_lcd_panel_disp_on_off(s_panel, true);
}

// GT911 reports in the panel's portrait 240x320 space. Mirroring x and then
// exchanging the axes gives landscape coordinates that match the panel's
// rotation (LilyGO's T-Deck examples set the same swap and mirror).
static esp_err_t touch_init(lv_display_t *disp)
{
    // The GT911 picks 0x5D or 0x14 at power-up from the level on its INT
    // line, and with no reset line here to force one, either can come up.
    // It may also still be starting after the power switch: give it time.
    uint16_t addr = 0;
    for (int attempt = 0; attempt < 10 && !addr; attempt++) {
        if (i2c_master_probe(board_main_i2c_bus(), ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS, 20) == ESP_OK) {
            addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS;
        } else if (i2c_master_probe(board_main_i2c_bus(), ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP, 20) == ESP_OK) {
            addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP;
        } else {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    ESP_RETURN_ON_FALSE(addr, ESP_ERR_NOT_FOUND, TAG, "no GT911 at 0x5D or 0x14");

    esp_lcd_panel_io_handle_t tio;
    esp_lcd_panel_io_i2c_config_t tio_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    tio_cfg.dev_addr = addr;
    tio_cfg.scl_speed_hz = 400000;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(board_main_i2c_bus(), &tio_cfg, &tio), TAG, "touch io");

    esp_lcd_touch_config_t tp_cfg = {
        .x_max = DISPLAY_V_RES,  // portrait width
        .y_max = DISPLAY_H_RES,  // portrait height
        .rst_gpio_num = -1,
        // Polled, as camillia-mt does on this board: with the INT line
        // configured, esp_lvgl_port reads only when it fires, and here it
        // never did (the line doubles as the GT911's address strap).
        .int_gpio_num = GPIO_NUM_NC,
        .flags = {
            .swap_xy = 1,
            .mirror_x = 1,
            .mirror_y = 0,
        },
    };
    esp_lcd_touch_handle_t tp;
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_gt911(tio, &tp_cfg, &tp), TAG, "gt911");

    const lvgl_port_touch_cfg_t touch_cfg = {.disp = disp, .handle = tp};
    ESP_RETURN_ON_FALSE(lvgl_port_add_touch(&touch_cfg), ESP_FAIL, TAG, "lvgl touch");
    ESP_LOGI(TAG, "GT911 ready at 0x%02x", addr);
    return ESP_OK;
}

esp_err_t display_init(void)
{
    ESP_RETURN_ON_ERROR(backlight_init(), TAG, "backlight");
    ESP_RETURN_ON_ERROR(panel_init(), TAG, "panel");

    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_stack = 8192;
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "lvgl port");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = s_io,
        .panel_handle = s_panel,
        // Partial rendering into two small internal DMA buffers: one is sent
        // over SPI while LVGL draws into the other.
        .buffer_size = DISPLAY_H_RES * DRAW_BUF_LINES,
        .double_buffer = true,
        .hres = DISPLAY_H_RES,
        .vres = DISPLAY_V_RES,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = {
            .swap_xy = true,
            .mirror_x = true,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = true,
            .swap_bytes = true,  // the ST7789 takes RGB565 big-endian
        },
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    ESP_RETURN_ON_FALSE(disp, ESP_FAIL, TAG, "lvgl display");

    // Touch is optional: the keyboard and trackball still drive the UI.
    esp_err_t terr = touch_init(disp);
    if (terr != ESP_OK) ESP_LOGE(TAG, "touch unavailable: %s", esp_err_to_name(terr));

    ESP_LOGI(TAG, "%dx%d LCD ready, %u bytes internal RAM free", DISPLAY_H_RES, DISPLAY_V_RES,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return ESP_OK;
}

esp_err_t display_set_test_pattern(bool on)
{
    return ESP_ERR_NOT_SUPPORTED;
}

bool display_test_pattern(void)
{
    return false;
}

#endif

void display_reveal(void)
{
    if (lvgl_port_lock(0)) {
        lv_refr_now(NULL);
        lvgl_port_unlock();
    }
    // Let the panel scan out the finished frame before lighting it.
    vTaskDelay(pdMS_TO_TICKS(50));
    display_set_brightness(BRIGHTNESS_DEFAULT);
}

bool display_lock(void)
{
    return lvgl_port_lock(0);
}

void display_unlock(void)
{
    lvgl_port_unlock();
}
