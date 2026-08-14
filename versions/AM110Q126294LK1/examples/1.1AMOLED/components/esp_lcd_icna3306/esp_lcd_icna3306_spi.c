/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <sys/cdefs.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_commands.h"
#include "esp_log.h"
#include "esp_lcd_icna3306.h"
#include "esp_lcd_icna3306_interface.h"

#define LCD_OPCODE_WRITE_CMD        (0x02ULL)
#define LCD_OPCODE_READ_CMD         (0x03ULL)
#define LCD_OPCODE_WRITE_COLOR      (0x32ULL)

static const char *TAG = "icna3306_spi";

static esp_err_t panel_icna3306_del(esp_lcd_panel_t *panel);
static esp_err_t panel_icna3306_reset(esp_lcd_panel_t *panel);
static esp_err_t panel_icna3306_init(esp_lcd_panel_t *panel);
static esp_err_t panel_icna3306_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end, const void *color_data);
static esp_err_t panel_icna3306_invert_color(esp_lcd_panel_t *panel, bool invert_color_data);
static esp_err_t panel_icna3306_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y);
static esp_err_t panel_icna3306_swap_xy(esp_lcd_panel_t *panel, bool swap_axes);
static esp_err_t panel_icna3306_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap);
static esp_err_t panel_icna3306_disp_on_off(esp_lcd_panel_t *panel, bool off);
static esp_err_t icna3306_spi_apply_brightness(void *driver_data, uint8_t brightness_percent);

typedef struct {
    esp_lcd_panel_t base;
    esp_lcd_panel_io_handle_t io;
    int reset_gpio_num;
    int x_gap;
    int y_gap;
    uint8_t fb_bits_per_pixel;
    uint8_t madctl_val; // save current value of LCD_CMD_MADCTL register
    uint8_t colmod_val; // save surrent value of LCD_CMD_COLMOD register
    icna3306_panel_context_t panel_ctx; // runtime context shared with public API
    const icna3306_lcd_init_cmd_t *init_cmds;
    uint16_t init_cmds_size;
    struct {
        unsigned int use_qspi_interface: 1;
        unsigned int reset_level: 1;
    } flags;
} icna3306_panel_t;

esp_err_t esp_lcd_new_panel_icna3306_spi(const esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *panel_dev_config, esp_lcd_panel_handle_t *ret_panel)
{
    ESP_RETURN_ON_FALSE(io && panel_dev_config && ret_panel, ESP_ERR_INVALID_ARG, TAG, "invalid argument");

    esp_err_t ret = ESP_OK;
    icna3306_panel_t *icna3306 = NULL;
    icna3306 = calloc(1, sizeof(icna3306_panel_t));
    ESP_GOTO_ON_FALSE(icna3306, ESP_ERR_NO_MEM, err, TAG, "no mem for icna3306 panel");

    if (panel_dev_config->reset_gpio_num >= 0) {
        gpio_config_t io_conf = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = 1ULL << panel_dev_config->reset_gpio_num,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&io_conf), err, TAG, "configure GPIO for RST line failed");
    }

    switch (panel_dev_config->rgb_ele_order) {
    case LCD_RGB_ELEMENT_ORDER_RGB:
        icna3306->madctl_val = 0;
        break;
    case LCD_RGB_ELEMENT_ORDER_BGR:
        icna3306->madctl_val |= LCD_CMD_BGR_BIT;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported color element order");
        break;
    }

    uint8_t fb_bits_per_pixel = 0;
    switch (panel_dev_config->bits_per_pixel) {
    case 16: // RGB565
        icna3306->colmod_val = 0x55;
        fb_bits_per_pixel = 16;
        break;
    case 18: // RGB666
        icna3306->colmod_val = 0x66;
        // each color component (R/G/B) should occupy the 6 high bits of a byte, which means 3 full bytes are required for a pixel
        fb_bits_per_pixel = 24;
        break;
    case 24: // RGB888
        icna3306->colmod_val = 0x77;
        fb_bits_per_pixel = 24;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported pixel width");
        break;
    }

    icna3306->io = io;
    icna3306->reset_gpio_num = panel_dev_config->reset_gpio_num;
    icna3306->fb_bits_per_pixel = fb_bits_per_pixel;
    icna3306_vendor_config_t *vendor_config = (icna3306_vendor_config_t *)panel_dev_config->vendor_config;
    if (vendor_config) {
        icna3306->init_cmds = vendor_config->init_cmds;
        icna3306->init_cmds_size = vendor_config->init_cmds_size;
        icna3306->flags.use_qspi_interface = vendor_config->flags.use_qspi_interface;
    }
    icna3306->flags.reset_level = panel_dev_config->flags.reset_active_high;
    icna3306->base.del = panel_icna3306_del;
    icna3306->base.reset = panel_icna3306_reset;
    icna3306->base.init = panel_icna3306_init;
    icna3306->base.draw_bitmap = panel_icna3306_draw_bitmap;
    icna3306->base.invert_color = panel_icna3306_invert_color;
    icna3306->base.set_gap = panel_icna3306_set_gap;
    icna3306->base.mirror = panel_icna3306_mirror;
    icna3306->base.swap_xy = panel_icna3306_swap_xy;
    icna3306->base.disp_on_off = panel_icna3306_disp_on_off;
    icna3306->panel_ctx.driver_data = icna3306;
    icna3306->panel_ctx.apply_brightness = icna3306_spi_apply_brightness;
    icna3306->base.user_data = &icna3306->panel_ctx;
    *ret_panel = &(icna3306->base);
    ESP_LOGD(TAG, "new icna3306 panel @%p", icna3306);

    return ESP_OK;

err:
    if (icna3306) {
        if (panel_dev_config->reset_gpio_num >= 0) {
            gpio_reset_pin(panel_dev_config->reset_gpio_num);
        }
        free(icna3306);
    }
    return ret;
}

static esp_err_t tx_param(icna3306_panel_t *icna3306, esp_lcd_panel_io_handle_t io, int lcd_cmd, const void *param, size_t param_size)
{
    if (icna3306->flags.use_qspi_interface) {
        lcd_cmd &= 0xff;
        lcd_cmd <<= 8;
        lcd_cmd |= LCD_OPCODE_WRITE_CMD << 24;
    }
    return esp_lcd_panel_io_tx_param(io, lcd_cmd, param, param_size);
}

static esp_err_t tx_color(icna3306_panel_t *icna3306, esp_lcd_panel_io_handle_t io, int lcd_cmd, const void *param, size_t param_size)
{
    if (icna3306->flags.use_qspi_interface) {
        lcd_cmd &= 0xff;
        lcd_cmd <<= 8;
        lcd_cmd |= LCD_OPCODE_WRITE_COLOR << 24;
    }
    return esp_lcd_panel_io_tx_color(io, lcd_cmd, param, param_size);
}

static esp_err_t panel_icna3306_del(esp_lcd_panel_t *panel)
{
    icna3306_panel_t *icna3306 = __containerof(panel, icna3306_panel_t, base);

    if (icna3306->reset_gpio_num >= 0) {
        gpio_reset_pin(icna3306->reset_gpio_num);
    }
    ESP_LOGD(TAG, "del icna3306 panel @%p", icna3306);
    free(icna3306);
    return ESP_OK;
}

static esp_err_t panel_icna3306_reset(esp_lcd_panel_t *panel)
{
    icna3306_panel_t *icna3306 = __containerof(panel, icna3306_panel_t, base);
    esp_lcd_panel_io_handle_t io = icna3306->io;

    // Perform hardware reset
    if (icna3306->reset_gpio_num >= 0) {
        gpio_set_level(icna3306->reset_gpio_num, icna3306->flags.reset_level);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(icna3306->reset_gpio_num, !icna3306->flags.reset_level);
        vTaskDelay(pdMS_TO_TICKS(150));
    } else { // Perform software reset
        ESP_RETURN_ON_ERROR(tx_param(icna3306, io, LCD_CMD_SWRESET, NULL, 0), TAG, "send command failed");
        vTaskDelay(pdMS_TO_TICKS(80));
    }

    return ESP_OK;
}

static const icna3306_lcd_init_cmd_t vendor_specific_init_default[] = {
//  {cmd, { data }, data_size, delay_ms}
    {0xFE, (uint8_t []){0x00}, 0, 0},
    {0xC4, (uint8_t []){0x80}, 1, 0},
    {0x35, (uint8_t []){0x00}, 0, 10},
    {0x53, (uint8_t []){0x20}, 1, 10},
    {0x51, (uint8_t []){0xFF}, 1, 10},
    {0x63, (uint8_t []){0xFF}, 1, 10},
    {0x2A, (uint8_t []){0x00, 0x06, 0x01, 0xDD}, 4, 0},
    {0x2B, (uint8_t []){0x00, 0x00, 0x01, 0xD1}, 4, 0},
    {0x11, (uint8_t []){0x00}, 0, 60},
    {0x29, (uint8_t []){0x00}, 0, 0},
};

static esp_err_t panel_icna3306_init(esp_lcd_panel_t *panel)
{
    icna3306_panel_t *icna3306 = __containerof(panel, icna3306_panel_t, base);
    esp_lcd_panel_io_handle_t io = icna3306->io;
    const icna3306_lcd_init_cmd_t *init_cmds = NULL;
    uint16_t init_cmds_size = 0;
    bool is_cmd_overwritten = false;

    // vendor specific initialization, it can be different between manufacturers
    // should consult the LCD supplier for initialization sequence code
    if (icna3306->init_cmds) {
        init_cmds = icna3306->init_cmds;
        init_cmds_size = icna3306->init_cmds_size;
        /* 有厂方完整序列时，严格按序发送，不再提前发 MADCTL/COLMOD，
         * 避免插在 FE-20 解锁 / FE-00 之前破坏时序 */
    } else {
        ESP_RETURN_ON_ERROR(tx_param(icna3306, io, LCD_CMD_MADCTL, (uint8_t[]) {
            icna3306->madctl_val,
        }, 1), TAG, "send command failed");
        ESP_RETURN_ON_ERROR(tx_param(icna3306, io, LCD_CMD_COLMOD, (uint8_t[]) {
            icna3306->colmod_val,
        }, 1), TAG, "send command failed");
        init_cmds = vendor_specific_init_default;
        init_cmds_size = sizeof(vendor_specific_init_default) / sizeof(icna3306_lcd_init_cmd_t);
    }

    for (int i = 0; i < init_cmds_size; i++) {
        ESP_LOGI(TAG, "init[%d/%u] cmd=0x%02X data_len=%u delay=%ums",
                 i + 1, init_cmds_size, init_cmds[i].cmd,
                 init_cmds[i].data_bytes, init_cmds[i].delay_ms);
        if (init_cmds[i].data && init_cmds[i].data_bytes > 0) {
            ESP_LOG_BUFFER_HEXDUMP(TAG, init_cmds[i].data, init_cmds[i].data_bytes, ESP_LOG_INFO);
        }

        // Check if the command has been used or conflicts with the internal
        switch (init_cmds[i].cmd) {
        case LCD_CMD_MADCTL:
            is_cmd_overwritten = true;
            if (init_cmds[i].data && init_cmds[i].data_bytes > 0) {
                icna3306->madctl_val = ((uint8_t *)init_cmds[i].data)[0];
            }
            break;
        case LCD_CMD_COLMOD:
            is_cmd_overwritten = true;
            if (init_cmds[i].data && init_cmds[i].data_bytes > 0) {
                icna3306->colmod_val = ((uint8_t *)init_cmds[i].data)[0];
            }
            break;
        default:
            is_cmd_overwritten = false;
            break;
        }

        if (is_cmd_overwritten) {
            ESP_LOGW(TAG, "The %02Xh command has been used and will be overwritten by external initialization sequence", init_cmds[i].cmd);
        }

        ESP_RETURN_ON_ERROR(tx_param(icna3306, io, init_cmds[i].cmd, init_cmds[i].data, init_cmds[i].data_bytes), TAG,
                            "send command failed");
        vTaskDelay(pdMS_TO_TICKS(init_cmds[i].delay_ms));
    }
    ESP_LOGI(TAG, "all %u initialization commands sent", init_cmds_size);

    return ESP_OK;
}

static esp_err_t panel_icna3306_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end, const void *color_data)
{
    icna3306_panel_t *icna3306 = __containerof(panel, icna3306_panel_t, base);
    assert((x_start < x_end) && (y_start < y_end) && "start position must be smaller than end position");
    esp_lcd_panel_io_handle_t io = icna3306->io;

    x_start += icna3306->x_gap;
    x_end += icna3306->x_gap;
    y_start += icna3306->y_gap;
    y_end += icna3306->y_gap;

    // define an area of frame memory where MCU can access
    ESP_RETURN_ON_ERROR(tx_param(icna3306, io, LCD_CMD_CASET, (uint8_t[]) {
        (x_start >> 8) & 0xFF,
        x_start & 0xFF,
        ((x_end - 1) >> 8) & 0xFF,
        (x_end - 1) & 0xFF,
    }, 4), TAG, "send command failed");
    ESP_RETURN_ON_ERROR(tx_param(icna3306, io, LCD_CMD_RASET, (uint8_t[]) {
        (y_start >> 8) & 0xFF,
        y_start & 0xFF,
        ((y_end - 1) >> 8) & 0xFF,
        (y_end - 1) & 0xFF,
    }, 4), TAG, "send command failed");
    // transfer frame buffer
    size_t len = (x_end - x_start) * (y_end - y_start) * icna3306->fb_bits_per_pixel / 8;
    ESP_RETURN_ON_ERROR(tx_color(icna3306, io, LCD_CMD_RAMWR, color_data, len), TAG, "send color data failed");

    return ESP_OK;
}

static esp_err_t panel_icna3306_invert_color(esp_lcd_panel_t *panel, bool invert_color_data)
{
    icna3306_panel_t *icna3306 = __containerof(panel, icna3306_panel_t, base);
    esp_lcd_panel_io_handle_t io = icna3306->io;
    int command = 0;
    if (invert_color_data) {
        command = LCD_CMD_INVON;
    } else {
        command = LCD_CMD_INVOFF;
    }
    ESP_RETURN_ON_ERROR(tx_param(icna3306, io, command, NULL, 0), TAG, "send command failed");
    return ESP_OK;
}

static esp_err_t panel_icna3306_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    icna3306_panel_t *icna3306 = __containerof(panel, icna3306_panel_t, base);
    esp_lcd_panel_io_handle_t io = icna3306->io;
    esp_err_t ret = ESP_OK;

    if (mirror_x) {
        icna3306->madctl_val |= BIT(6);
    } else {
        icna3306->madctl_val &= ~BIT(6);
    }
    if (mirror_y) {
        ESP_LOGE(TAG, "mirror_y is not supported by this panel");
        ret = ESP_ERR_NOT_SUPPORTED;
    }
    ESP_RETURN_ON_ERROR(tx_param(icna3306, io, LCD_CMD_MADCTL, (uint8_t[]) {
        icna3306->madctl_val
    }, 1), TAG, "send command failed");
    return ret;
}

static esp_err_t panel_icna3306_swap_xy(esp_lcd_panel_t *panel, bool swap_axes)
{
    ESP_LOGE(TAG, "swap_xy is not supported by this panel");
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t panel_icna3306_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap)
{
    icna3306_panel_t *icna3306 = __containerof(panel, icna3306_panel_t, base);
    icna3306->x_gap = x_gap;
    icna3306->y_gap = y_gap;
    return ESP_OK;
}

static esp_err_t panel_icna3306_disp_on_off(esp_lcd_panel_t *panel, bool on_off)
{
    icna3306_panel_t *icna3306 = __containerof(panel, icna3306_panel_t, base);
    esp_lcd_panel_io_handle_t io = icna3306->io;
    int command = 0;

    if (on_off) {
        command = LCD_CMD_DISPON;
    } else {
        command = LCD_CMD_DISPOFF;
    }
    ESP_RETURN_ON_ERROR(tx_param(icna3306, io, command, NULL, 0), TAG, "send command failed");
    return ESP_OK;
}

static esp_err_t icna3306_spi_apply_brightness(void *driver_data, uint8_t brightness_percent)
{
    icna3306_panel_t *icna3306 = (icna3306_panel_t *)driver_data;
    ESP_RETURN_ON_FALSE(icna3306, ESP_ERR_INVALID_ARG, TAG, "invalid panel data");

    esp_lcd_panel_io_handle_t io = icna3306->io;
    ESP_RETURN_ON_FALSE(io, ESP_ERR_INVALID_STATE, TAG, "panel IO not initialized");

    uint8_t hw_brightness = (brightness_percent * 255) / 100;

    ESP_RETURN_ON_ERROR(tx_param(icna3306, io, 0x51, (uint8_t[]) {
        hw_brightness
    }, 1), TAG, "send brightness command failed");

    ESP_LOGI(TAG, "set brightness to %d%% (hardware value: %d)", brightness_percent, hw_brightness);

    return ESP_OK;
}
