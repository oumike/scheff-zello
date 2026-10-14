/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 *
 * NV3031B panel driver, after Espressif's esp_lcd_sh8601 as adapted by the
 * Tactility project (Drivers/nv3031b-module). The vendor init table is the
 * one LovyanGFX's Panel_NV3031B sends (BSD-2-Clause).
 */

#include "lcd_nv3031b.h"

#include <stdlib.h>
#include <sys/cdefs.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_interface.h"

static const char *TAG = "nv3031b";

// QSPI framing: an opcode byte, then the command in the middle of a 24-bit
// address. Commands and parameters go out on one line, pixels on four.
#define OPCODE_WRITE_CMD   0x02
#define OPCODE_WRITE_COLOR 0x32

typedef struct {
    uint8_t cmd;
    uint8_t len;
    uint8_t data[8];
} init_cmd_t;

static const init_cmd_t s_init_cmds[] = {
    {0xFD, 2, {0x06, 0x08}},  // vendor command set on
    {0x60, 1, {0x0C}},
    {0x61, 2, {0x07, 0x04}},
    {0xB4, 1, {0x01}},
    {0xB1, 3, {0x0F, 0x02, 0x03}},
    {0xB5, 4, {0x02, 0x02, 0x0A, 0x14}},
    {0xB6, 5, {0x44, 0x01, 0x9F, 0x00, 0x02}},
    {0xDF, 1, {0x11}},
    {0x67, 1, {0x21}},
    {0x68, 4, {0x90, 0x4F, 0x27, 0x21}},
    {0xE1, 2, {0x20, 0x69}},
    {0xE4, 2, {0x69, 0x20}},
    {0xE2, 6, {0x10, 0x12, 0x12, 0x30, 0x39, 0x3F}},  // gamma
    {0xE5, 6, {0x3F, 0x33, 0x2D, 0x12, 0x12, 0x10}},
    {0xE0, 8, {0x06, 0x06, 0x0B, 0x12, 0x11, 0x11, 0x0E, 0x19}},
    {0xE3, 8, {0x19, 0x13, 0x14, 0x14, 0x14, 0x12, 0x08, 0x05}},
    {0xE6, 2, {0x00, 0xFF}},
    {0xE7, 6, {0x01, 0x04, 0x03, 0x03, 0x00, 0x12}},
    {0xE8, 3, {0x00, 0x70, 0x00}},
    {0xEC, 1, {0x54}},
    {0xFD, 2, {0xFA, 0xFC}},  // vendor command set off
};

typedef struct {
    esp_lcd_panel_t base;
    esp_lcd_panel_io_handle_t io;
    int x_gap, y_gap;
    uint8_t madctl;
    uint8_t colmod;
    uint8_t bytes_per_pixel;
} nv3031b_t;

static esp_err_t tx_param(esp_lcd_panel_io_handle_t io, uint8_t cmd, const void *param, size_t len)
{
    return esp_lcd_panel_io_tx_param(io, (OPCODE_WRITE_CMD << 24) | (cmd << 8), param, len);
}

static esp_err_t send_madctl(nv3031b_t *p)
{
    return tx_param(p->io, LCD_CMD_MADCTL, &p->madctl, 1);
}

static esp_err_t panel_del(esp_lcd_panel_t *panel)
{
    free(__containerof(panel, nv3031b_t, base));
    return ESP_OK;
}

static esp_err_t panel_reset(esp_lcd_panel_t *panel)
{
    nv3031b_t *p = __containerof(panel, nv3031b_t, base);
    ESP_RETURN_ON_ERROR(tx_param(p->io, LCD_CMD_SWRESET, NULL, 0), TAG, "swreset");
    vTaskDelay(pdMS_TO_TICKS(120));
    return ESP_OK;
}

static esp_err_t panel_init(esp_lcd_panel_t *panel)
{
    nv3031b_t *p = __containerof(panel, nv3031b_t, base);
    // A NOP first resynchronizes the QSPI interface.
    ESP_RETURN_ON_ERROR(tx_param(p->io, LCD_CMD_NOP, NULL, 0), TAG, "nop");
    for (size_t i = 0; i < sizeof(s_init_cmds) / sizeof(s_init_cmds[0]); i++) {
        const init_cmd_t *c = &s_init_cmds[i];
        ESP_RETURN_ON_ERROR(tx_param(p->io, c->cmd, c->data, c->len), TAG, "init 0x%02x", c->cmd);
    }
    ESP_RETURN_ON_ERROR(tx_param(p->io, LCD_CMD_COLMOD, &p->colmod, 1), TAG, "colmod");
    // Sleep out once only: on this controller leaving sleep a second time
    // shifts the image a column, so the panel is never put back to sleep.
    ESP_RETURN_ON_ERROR(tx_param(p->io, LCD_CMD_SLPOUT, NULL, 0), TAG, "slpout");
    vTaskDelay(pdMS_TO_TICKS(120));
    return send_madctl(p);
}

static esp_err_t panel_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end,
                                   const void *color_data)
{
    nv3031b_t *p = __containerof(panel, nv3031b_t, base);
    x_start += p->x_gap;
    x_end += p->x_gap;
    y_start += p->y_gap;
    y_end += p->y_gap;
    uint8_t col[4] = {x_start >> 8, x_start & 0xff, (x_end - 1) >> 8, (x_end - 1) & 0xff};
    uint8_t row[4] = {y_start >> 8, y_start & 0xff, (y_end - 1) >> 8, (y_end - 1) & 0xff};
    ESP_RETURN_ON_ERROR(tx_param(p->io, LCD_CMD_CASET, col, 4), TAG, "caset");
    ESP_RETURN_ON_ERROR(tx_param(p->io, LCD_CMD_RASET, row, 4), TAG, "raset");
    size_t len = (size_t)(x_end - x_start) * (y_end - y_start) * p->bytes_per_pixel;
    return esp_lcd_panel_io_tx_color(p->io, (OPCODE_WRITE_COLOR << 24) | (LCD_CMD_RAMWR << 8), color_data, len);
}

static esp_err_t panel_invert_color(esp_lcd_panel_t *panel, bool invert)
{
    nv3031b_t *p = __containerof(panel, nv3031b_t, base);
    return tx_param(p->io, invert ? LCD_CMD_INVON : LCD_CMD_INVOFF, NULL, 0);
}

static esp_err_t panel_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    nv3031b_t *p = __containerof(panel, nv3031b_t, base);
    p->madctl &= ~(LCD_CMD_MX_BIT | LCD_CMD_MY_BIT);
    if (mirror_x) p->madctl |= LCD_CMD_MX_BIT;
    if (mirror_y) p->madctl |= LCD_CMD_MY_BIT;
    return send_madctl(p);
}

static esp_err_t panel_swap_xy(esp_lcd_panel_t *panel, bool swap)
{
    nv3031b_t *p = __containerof(panel, nv3031b_t, base);
    p->madctl &= ~LCD_CMD_MV_BIT;
    if (swap) p->madctl |= LCD_CMD_MV_BIT;
    return send_madctl(p);
}

static esp_err_t panel_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap)
{
    nv3031b_t *p = __containerof(panel, nv3031b_t, base);
    p->x_gap = x_gap;
    p->y_gap = y_gap;
    return ESP_OK;
}

static esp_err_t panel_disp_on_off(esp_lcd_panel_t *panel, bool on)
{
    nv3031b_t *p = __containerof(panel, nv3031b_t, base);
    if (!on) return tx_param(p->io, LCD_CMD_DISPOFF, NULL, 0);
    // DISPON takes one dummy parameter byte on this controller.
    static const uint8_t dummy = 0;
    return tx_param(p->io, LCD_CMD_DISPON, &dummy, 1);
}

esp_err_t lcd_new_panel_nv3031b(esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *config,
                                esp_lcd_panel_handle_t *ret_panel)
{
    ESP_RETURN_ON_FALSE(io && config && ret_panel, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(config->bits_per_pixel == 16, ESP_ERR_NOT_SUPPORTED, TAG, "only RGB565");
    nv3031b_t *p = calloc(1, sizeof(*p));
    ESP_RETURN_ON_FALSE(p, ESP_ERR_NO_MEM, TAG, "no memory");

    p->io = io;
    p->colmod = 0x55;  // RGB565
    p->bytes_per_pixel = 2;
    p->madctl = config->rgb_ele_order == LCD_RGB_ELEMENT_ORDER_BGR ? LCD_CMD_BGR_BIT : 0;
    p->base.del = panel_del;
    p->base.reset = panel_reset;
    p->base.init = panel_init;
    p->base.draw_bitmap = panel_draw_bitmap;
    p->base.invert_color = panel_invert_color;
    p->base.mirror = panel_mirror;
    p->base.swap_xy = panel_swap_xy;
    p->base.set_gap = panel_set_gap;
    p->base.disp_on_off = panel_disp_on_off;
    *ret_panel = &p->base;
    return ESP_OK;
}
