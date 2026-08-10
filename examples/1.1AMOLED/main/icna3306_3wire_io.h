#pragma once

#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t icna3306_new_panel_io_3wire(spi_host_device_t host,
                                      int cs_gpio_num,
                                      int pclk_hz,
                                      esp_lcd_panel_io_handle_t *ret_io);

#ifdef __cplusplus
}
#endif
