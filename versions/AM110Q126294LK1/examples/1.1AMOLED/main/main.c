#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_heap_caps.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_icna3306.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_chsc6417.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"

#include "esp_lv_adapter.h"
#include "lvgl.h"
#include "lv_demos.h"

#include "icna3306_init_cmds.h"

static const char *TAG = "LVGL_ICNA3306_CHSC6417_SPI";

#define EXAMPLE_PIN_NUM_LCD_SDO           (GPIO_NUM_16)
#define EXAMPLE_PIN_NUM_LCD_SDI           (GPIO_NUM_15)
#define EXAMPLE_PIN_NUM_LCD_DC            (GPIO_NUM_14)
#define EXAMPLE_PIN_NUM_LCD_PCLK          (GPIO_NUM_13)
#define EXAMPLE_PIN_NUM_LCD_CS            (GPIO_NUM_12)
#define EXAMPLE_PIN_NUM_LCD_TE            (GPIO_NUM_38)
#define EXAMPLE_PIN_NUM_LCD_RST           (GPIO_NUM_47)


#define EXAMPLE_TOUCH_I2C_NUM       I2C_NUM_0
#define EXAMPLE_TOUCH_I2C_CLK_HZ    (400 * 1000)
#define EXAMPLE_PIN_NUM_TOUCH_INT         (GPIO_NUM_42)
#define EXAMPLE_PIN_NUM_TOUCH_SCL         (GPIO_NUM_41)
#define EXAMPLE_PIN_NUM_TOUCH_SDA         (GPIO_NUM_40)
#define EXAMPLE_PIN_NUM_TOUCH_RST         (GPIO_NUM_39)


#define EXAMPLE_LCD_HOST            SPI2_HOST
#define EXAMPLE_LCD_PIXEL_CLOCK_HZ  (20 * 1000 * 1000)

#define EXAMPLE_LCD_H_RES              126
#define EXAMPLE_LCD_V_RES              294
#define EXAMPLE_LCD_X_GAP              2
#define EXAMPLE_LCD_Y_GAP              0

static void icna3306_area_rounder_cb(lv_area_t *area, void *user_data)
{
    uint16_t x1 = area->x1;
    uint16_t x2 = area->x2;
    uint16_t y1 = area->y1;
    uint16_t y2 = area->y2;

    area->x1 = (x1 >> 1) << 1;
    area->y1 = (y1 >> 1) << 1;
    area->x2 = ((x2 >> 1) << 1) + 1;
    area->y2 = ((y2 >> 1) << 1) + 1;
}

static esp_err_t lcd_wait_color_transfer_done(esp_lcd_panel_io_handle_t io)
{
    /* tx_param is polling and waits for all queued tx_color DMA transactions first. */
    return esp_lcd_panel_io_tx_param(io, LCD_CMD_NOP, NULL, 0);
}

static esp_err_t lcd_fill_solid_frame(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t io, uint16_t color)
{
    const size_t pixels = (size_t)EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES;
    uint16_t *fb = heap_caps_malloc(pixels * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!fb) {
        uint16_t *line = heap_caps_malloc(EXAMPLE_LCD_H_RES * sizeof(uint16_t), MALLOC_CAP_DMA);
        ESP_RETURN_ON_FALSE(line, ESP_ERR_NO_MEM, TAG, "no mem for line buffer");
        for (int x = 0; x < EXAMPLE_LCD_H_RES; x++) {
            line[x] = color;
        }
        for (int y = 0; y < EXAMPLE_LCD_V_RES; y++) {
            esp_err_t err = esp_lcd_panel_draw_bitmap(panel, 0, y, EXAMPLE_LCD_H_RES, y + 1, line);
            if (err == ESP_OK) {
                err = lcd_wait_color_transfer_done(io);
            }
            if (err != ESP_OK) {
                free(line);
                return err;
            }
        }
        free(line);
        return ESP_OK;
    }

    for (size_t i = 0; i < pixels; i++) {
        fb[i] = color;
    }
    esp_err_t err = esp_lcd_panel_draw_bitmap(panel, 0, 0, EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES, fb);
    if (err == ESP_OK) {
        err = lcd_wait_color_transfer_done(io);
    }
    free(fb);
    return err;
}

/* Read the three CMD1 ID registers separately. */
static void lcd_probe_id(esp_lcd_panel_io_handle_t io)
{
    static const uint8_t id_cmds[] = {0xDA, 0xDB, 0xDC};
    uint8_t id[3] = {0};
    bool read_ok = true;
    for (size_t i = 0; i < sizeof(id_cmds); i++) {
        esp_err_t err = esp_lcd_panel_io_rx_param(io, id_cmds[i], &id[i], 1);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "read ID command 0x%02X failed: %s", id_cmds[i], esp_err_to_name(err));
            read_ok = false;
        }
    }
    ESP_LOGI(TAG, "panel CMD1 ID: %02X %02X %02X %s",
             id[0], id[1], id[2],
             read_ok && (id[0] | id[1] | id[2]) ? "(read OK)" : "(not confirmed)");
}

static void lcd_probe_power_mode(esp_lcd_panel_io_handle_t io, const char *stage)
{
    uint8_t power_mode = 0;
    esp_err_t err = esp_lcd_panel_io_rx_param(io, 0x0A, &power_mode, 1);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%s: RDDPM(0x0A)=0x%02X [booster=%u sleep_out=%u normal=%u display_on=%u]",
                 stage, power_mode,
                 !!(power_mode & BIT(7)), !!(power_mode & BIT(4)),
                 !!(power_mode & BIT(3)), !!(power_mode & BIT(2)));
    } else {
        ESP_LOGW(TAG, "%s: RDDPM read failed: %s", stage, esp_err_to_name(err));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32-S3 + ICNA3306 (4-wire SPI) + CHSC6417 + LVGL8 开始初始化...");
    ESP_LOGI(TAG, "LCD: %dx%d RGB565, SPI2 %uHz, gap=(%d,%d)",
             EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES, EXAMPLE_LCD_PIXEL_CLOCK_HZ,
             EXAMPLE_LCD_X_GAP, EXAMPLE_LCD_Y_GAP);
    ESP_LOGI(TAG, "LCD pins: SDO=%d SDI=%d DC=%d CLK=%d CS=%d TE=%d RST=%d",
             EXAMPLE_PIN_NUM_LCD_SDO, EXAMPLE_PIN_NUM_LCD_SDI, EXAMPLE_PIN_NUM_LCD_DC,
             EXAMPLE_PIN_NUM_LCD_PCLK, EXAMPLE_PIN_NUM_LCD_CS, EXAMPLE_PIN_NUM_LCD_TE,
             EXAMPLE_PIN_NUM_LCD_RST);
    ESP_LOGI(TAG, "Touch pins: INT=%d SCL=%d SDA=%d RST=%d, I2C=%uHz",
             EXAMPLE_PIN_NUM_TOUCH_INT, EXAMPLE_PIN_NUM_TOUCH_SCL,
             EXAMPLE_PIN_NUM_TOUCH_SDA, EXAMPLE_PIN_NUM_TOUCH_RST,
             EXAMPLE_TOUCH_I2C_CLK_HZ);
    ESP_LOGI(TAG, "Heap before init: internal=%u bytes, PSRAM=%u bytes",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    // 1. SPI 总线初始化（4-wire：CLK/SDI/SDO，DC 在 Panel IO 中配置）
    spi_bus_config_t bus_config = ICNA3306_PANEL_BUS_SPI_CONFIG(
        EXAMPLE_PIN_NUM_LCD_PCLK,
        EXAMPLE_PIN_NUM_LCD_SDI,
        EXAMPLE_LCD_H_RES * 80 * sizeof(uint16_t)
    );
    bus_config.miso_io_num = EXAMPLE_PIN_NUM_LCD_SDO;
    ESP_ERROR_CHECK(spi_bus_initialize(EXAMPLE_LCD_HOST, &bus_config, SPI_DMA_CH_AUTO));
    ESP_LOGI(TAG, "SPI2 bus ready, max_transfer=%u bytes", (unsigned)bus_config.max_transfer_sz);

    // 2. LCD Panel IO (4-wire SPI，DC 区分命令/数据)
    esp_lcd_panel_io_spi_config_t io_config = ICNA3306_PANEL_IO_SPI_CONFIG(
        EXAMPLE_PIN_NUM_LCD_CS,
        EXAMPLE_PIN_NUM_LCD_DC,
        NULL,
        NULL
    );
    io_config.pclk_hz = EXAMPLE_LCD_PIXEL_CLOCK_HZ;
    esp_lcd_panel_io_handle_t panel_io = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)EXAMPLE_LCD_HOST, &io_config, &panel_io));
    ESP_LOGI(TAG, "LCD panel IO ready: mode=%d, queue_depth=%d", io_config.spi_mode, io_config.trans_queue_depth);

    // 3. ICNA3306 Panel 初始化（SPI，非 QSPI）
    icna3306_vendor_config_t vendor_config = {
        .init_cmds = lcd_init_cmds,
        .init_cmds_size = lcd_init_cmds_size,
        .flags = {
            .use_qspi_interface = 0,
        },
    };
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = EXAMPLE_PIN_NUM_LCD_RST,
        .rgb_endian = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_config,
    };

    esp_lcd_panel_handle_t panel_handle = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_icna3306(panel_io, &panel_config, &panel_handle));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_LOGI(TAG, "ICNA3306 hardware reset completed");
    vTaskDelay(pdMS_TO_TICKS(20));
    lcd_probe_id(panel_io);

    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_LOGI(TAG, "ICNA3306 vendor initialization completed through Display On (0x29)");
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel_handle, EXAMPLE_LCD_X_GAP, EXAMPLE_LCD_Y_GAP));

    ESP_ERROR_CHECK(lcd_fill_solid_frame(panel_handle, panel_io, 0xFFFF));
    ESP_LOGI(TAG, "White first frame DMA transfer actually completed: %u pixels, %u bytes",
             EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES,
             EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES * (unsigned)sizeof(uint16_t));
    ESP_LOGW(TAG, "完整首帧 DMA 已结束：现在立即进入 ELVDD/ELVSS 上电阶段，不增加额外延时");
    lcd_probe_power_mode(panel_io, "完整首帧发送完成");

    ESP_LOGI(TAG, "ICNA3306 SPI 显示初始化完成 (%dx%d), gap=(%d,%d)",
             EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES, EXAMPLE_LCD_X_GAP, EXAMPLE_LCD_Y_GAP);

    // 4. CHSC6417 触摸初始化（I2C）
    i2c_master_bus_config_t i2c_bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = EXAMPLE_TOUCH_I2C_NUM,
        .scl_io_num = EXAMPLE_PIN_NUM_TOUCH_SCL,
        .sda_io_num = EXAMPLE_PIN_NUM_TOUCH_SDA,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t i2c_bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_config, &i2c_bus));
    ESP_LOGI(TAG, "Touch I2C bus ready");

    esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_CHSC6417_CONFIG();
    tp_io_config.scl_speed_hz = EXAMPLE_TOUCH_I2C_CLK_HZ;

    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus, &tp_io_config, &tp_io_handle));

    esp_lcd_touch_config_t tp_cfg = {
        .x_max = EXAMPLE_LCD_H_RES,
        .y_max = EXAMPLE_LCD_V_RES,
        .rst_gpio_num = EXAMPLE_PIN_NUM_TOUCH_RST,
        .int_gpio_num = EXAMPLE_PIN_NUM_TOUCH_INT,
        .levels = {.reset = 0, .interrupt = 0},
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };
    esp_lcd_touch_handle_t touch_handle = NULL;
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_chsc6417(tp_io_handle, &tp_cfg, &touch_handle));

    ESP_LOGI(TAG, "CHSC6417 触控初始化完成");

    // 5. esp_lvgl_adapter 初始化
    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_cfg.stack_in_psram = true; // 尝试将任务栈放入PSRAM
    ESP_ERROR_CHECK(esp_lv_adapter_init(&adapter_cfg));

    // 注册显示器（SPI）
    esp_lv_adapter_display_config_t disp_cfg =
        ESP_LV_ADAPTER_DISPLAY_SPI_WITH_PSRAM_DEFAULT_CONFIG(
            panel_handle,
            panel_io,
            EXAMPLE_LCD_H_RES,
            EXAMPLE_LCD_V_RES,
            ESP_LV_ADAPTER_ROTATE_0);   // 根据实际画面方向修改（0/90/180/270）

    lv_display_t *disp = esp_lv_adapter_register_display(&disp_cfg);
    assert(disp != NULL);

    // 设置背景颜色为黑色
    lv_obj_t *default_scr = lv_disp_get_scr_act(disp);     // 获取当前活动屏幕
    if (default_scr) {
        lv_obj_remove_style_all(default_scr);               // 清除默认样式
        lv_obj_set_style_bg_color(default_scr, lv_color_black(), LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(default_scr, LV_OPA_COVER, LV_STATE_DEFAULT);
        lv_obj_set_size(default_scr, LV_PCT(100), LV_PCT(100));
    }

    //注册对齐回调
    ESP_ERROR_CHECK(esp_lv_adapter_set_area_rounder_cb(disp, icna3306_area_rounder_cb, NULL));

    // 注册触摸输入设备
    esp_lv_adapter_touch_config_t touch_cfg = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, touch_handle);
    lv_indev_t *indev = esp_lv_adapter_register_touch(&touch_cfg);
    assert(indev != NULL);

    ESP_ERROR_CHECK(esp_lv_adapter_start());
    ESP_LOGI(TAG, "esp_lvgl_adapter 启动成功！");

    // 6. 创建 LVGL UI（必须在锁内操作）
    if (esp_lv_adapter_lock(portMAX_DELAY) == ESP_OK) {

        lv_demo_widgets();           // 官方 widget demo
        // lv_demo_music();             // 音乐 demo
        // lv_demo_benchmark();         // 性能测试

        esp_lv_adapter_unlock();
    }

    ESP_LOGW(TAG, "请测量 TP1=ELVDD、TP2=ELVSS；目标约 +4.6V / -2.4V");
    ESP_LOGI(TAG, "LVGL UI 创建完成，系统正常运行...");
}
