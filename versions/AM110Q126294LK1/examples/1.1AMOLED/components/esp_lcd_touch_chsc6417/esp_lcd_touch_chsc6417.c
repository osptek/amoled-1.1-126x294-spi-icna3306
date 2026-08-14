/*
 * SPDX-FileCopyrightText: 2015-2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_system.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_rom_sys.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"

#include "esp_lcd_touch_chsc6417.h"

#define POINT_NUM_MAX       (1)

#define DATA_START_REG      (0x00)
#define CHIP_ID_REG         (0xA7)

static const char *TAG = "CHSC6417";

static esp_err_t read_data(esp_lcd_touch_handle_t tp);
static bool get_xy(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y, uint16_t *strength, uint8_t *point_num, uint8_t max_point_num);
static esp_err_t del(esp_lcd_touch_handle_t tp);

static esp_err_t i2c_read_bytes(esp_lcd_touch_handle_t tp, uint16_t reg, uint8_t *data, uint8_t len);

static esp_err_t reset(esp_lcd_touch_handle_t tp);
static esp_err_t read_id(esp_lcd_touch_handle_t tp);

esp_err_t esp_lcd_touch_new_i2c_chsc6417(const esp_lcd_panel_io_handle_t io, const esp_lcd_touch_config_t *config, esp_lcd_touch_handle_t *tp)
{
    ESP_RETURN_ON_FALSE(io, ESP_ERR_INVALID_ARG, TAG, "Invalid io");
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "Invalid config");
    ESP_RETURN_ON_FALSE(tp, ESP_ERR_INVALID_ARG, TAG, "Invalid touch handle");

    esp_err_t ret = ESP_OK;
    esp_lcd_touch_handle_t chsc6417 = calloc(1, sizeof(esp_lcd_touch_t));
    ESP_GOTO_ON_FALSE(chsc6417, ESP_ERR_NO_MEM, err, TAG, "Touch handle malloc failed");

    chsc6417->io = io;
    chsc6417->read_data = read_data;
    chsc6417->get_xy = get_xy;
    chsc6417->del = del;
    chsc6417->data.lock.owner = portMUX_FREE_VAL;
    memcpy(&chsc6417->config, config, sizeof(esp_lcd_touch_config_t));

    if (chsc6417->config.int_gpio_num != GPIO_NUM_NC) {
        const gpio_config_t int_gpio_config = {
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .intr_type = (chsc6417->config.levels.interrupt ? GPIO_INTR_POSEDGE : GPIO_INTR_NEGEDGE),
            .pin_bit_mask = BIT64(chsc6417->config.int_gpio_num)
        };
        ESP_GOTO_ON_ERROR(gpio_config(&int_gpio_config), err, TAG, "GPIO intr config failed");

        if (chsc6417->config.interrupt_callback) {
            esp_lcd_touch_register_interrupt_callback(chsc6417, chsc6417->config.interrupt_callback);
        }
    }

    if (chsc6417->config.rst_gpio_num != GPIO_NUM_NC) {
        const gpio_config_t rst_gpio_config = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = BIT64(chsc6417->config.rst_gpio_num)
        };
        ESP_GOTO_ON_ERROR(gpio_config(&rst_gpio_config), err, TAG, "GPIO reset config failed");
    }

    ESP_GOTO_ON_ERROR(reset(chsc6417), err, TAG, "Reset failed");
    ESP_LOGI(TAG, "reset complete: INT=%d RST=%d max=%ux%u swap_xy=%u mirror_x=%u mirror_y=%u",
             chsc6417->config.int_gpio_num, chsc6417->config.rst_gpio_num,
             chsc6417->config.x_max, chsc6417->config.y_max,
             chsc6417->config.flags.swap_xy, chsc6417->config.flags.mirror_x,
             chsc6417->config.flags.mirror_y);
    esp_err_t id_err = read_id(chsc6417);
    if (id_err != ESP_OK) {
        ESP_LOGW(TAG, "chip ID read failed, touch polling will continue: %s", esp_err_to_name(id_err));
    }
    *tp = chsc6417;

    ESP_LOGI(TAG, "LCD touch panel create success, version: %d.%d.%d",
             ESP_LCD_TOUCH_CHSC6417_VER_MAJOR,
             ESP_LCD_TOUCH_CHSC6417_VER_MINOR,
             ESP_LCD_TOUCH_CHSC6417_VER_PATCH);

    return ESP_OK;
err:
    if (chsc6417) {
        del(chsc6417);
    }
    ESP_LOGE(TAG, "Initialization failed!");
    return ret;
}

static esp_err_t read_data(esp_lcd_touch_handle_t tp)
{
    static bool was_pressed = false;
    static uint16_t last_x = 0;
    static uint16_t last_y = 0;
    uint8_t lvalue[3] = {0};
    uint16_t x = 0;
    uint16_t y = 0;
    uint8_t point_num = 0;

    /*
     * CHSC6417 触摸数据读取（对齐 Makerfabs 实测协议）：
     *   1) 向 reg 0x5C 写入 0xE0
     *   2) 从 reg 0x5D 读 3 字节
     * 原库直接 rx 0xE0 在空闲轮询时会 NACK，导致 LVGL 刷屏报错。
     */
    const uint8_t touch_cmd = 0xE0;
    esp_err_t err = esp_lcd_panel_io_tx_param(tp->io, 0x5C, &touch_cmd, 1);
    if (err == ESP_OK) {
        esp_rom_delay_us(1000);
        err = i2c_read_bytes(tp, 0x5D, lvalue, sizeof(lvalue));
    }

    if (err == ESP_OK) {
        point_num = lvalue[0] & 0x03;
        x = (uint16_t)((((lvalue[0] & 0x40) >> 6) << 8) | lvalue[1]);
        y = (uint16_t)((((lvalue[0] & 0x80) >> 7) << 8) | lvalue[2]);
    } else {
        /* 无触摸/忙时 NACK：当作无点，避免刷屏报错 */
        point_num = 0;
        x = 0;
        y = 0;
    }

    point_num = (point_num > POINT_NUM_MAX ? POINT_NUM_MAX : point_num);
    if (point_num > 0) {
        uint16_t delta_x = (x > last_x) ? (x - last_x) : (last_x - x);
        uint16_t delta_y = (y > last_y) ? (y - last_y) : (last_y - y);
        if (!was_pressed) {
            ESP_LOGI(TAG, "touch DOWN raw=(%" PRIu16 ",%" PRIu16 ") packet=%02X %02X %02X",
                     x, y, lvalue[0], lvalue[1], lvalue[2]);
        } else if (delta_x >= 2 || delta_y >= 2) {
            ESP_LOGI(TAG, "touch MOVE raw=(%" PRIu16 ",%" PRIu16 ") delta=(%" PRIu16 ",%" PRIu16 ")",
                     x, y, delta_x, delta_y);
        }
        was_pressed = true;
        last_x = x;
        last_y = y;
    } else if (was_pressed) {
        ESP_LOGI(TAG, "touch UP last=(%" PRIu16 ",%" PRIu16 ")", last_x, last_y);
        was_pressed = false;
    }

    portENTER_CRITICAL(&tp->data.lock);
    tp->data.points = point_num;
    for (int i = 0; i < point_num; i++) {
        tp->data.coords[i].x = x;
        tp->data.coords[i].y = y;
    }
    portEXIT_CRITICAL(&tp->data.lock);

    return ESP_OK;
}

static bool get_xy(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y, uint16_t *strength, uint8_t *point_num, uint8_t max_point_num)
{
    portENTER_CRITICAL(&tp->data.lock);
    *point_num = (tp->data.points > max_point_num ? max_point_num : tp->data.points);
    for (size_t i = 0; i < *point_num; i++) {
        x[i] = tp->data.coords[i].x;
        y[i] = tp->data.coords[i].y;

        if (strength) {
            strength[i] = tp->data.coords[i].strength;
        }
    }
    tp->data.points = 0;
    portEXIT_CRITICAL(&tp->data.lock);

    return (*point_num > 0);
}

static esp_err_t del(esp_lcd_touch_handle_t tp)
{
    if (tp->config.int_gpio_num != GPIO_NUM_NC) {
        gpio_reset_pin(tp->config.int_gpio_num);
        if (tp->config.interrupt_callback) {
            gpio_isr_handler_remove(tp->config.int_gpio_num);
        }
    }
    if (tp->config.rst_gpio_num != GPIO_NUM_NC) {
        gpio_reset_pin(tp->config.rst_gpio_num);
    }
    free(tp);

    return ESP_OK;
}

static esp_err_t reset(esp_lcd_touch_handle_t tp)
{
    if (tp->config.rst_gpio_num != GPIO_NUM_NC) {
        ESP_RETURN_ON_ERROR(gpio_set_level(tp->config.rst_gpio_num, tp->config.levels.reset), TAG, "GPIO set level failed");
        vTaskDelay(pdMS_TO_TICKS(200));
        ESP_RETURN_ON_ERROR(gpio_set_level(tp->config.rst_gpio_num, !tp->config.levels.reset), TAG, "GPIO set level failed");
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    return ESP_OK;
}

static esp_err_t read_id(esp_lcd_touch_handle_t tp)
{
    uint8_t id;
    ESP_RETURN_ON_ERROR(i2c_read_bytes(tp, CHIP_ID_REG, &id, 1), TAG, "I2C read failed");
    ESP_LOGI(TAG, "chip ID register 0x%02X = 0x%02X", CHIP_ID_REG, id);
    return ESP_OK;
}

static esp_err_t i2c_read_bytes(esp_lcd_touch_handle_t tp, uint16_t reg, uint8_t *data, uint8_t len)
{
    ESP_RETURN_ON_FALSE(data, ESP_ERR_INVALID_ARG, TAG, "Invalid data");
    return esp_lcd_panel_io_rx_param(tp->io, reg, data, len);
}
