#ifndef ICNA3306_INIT_CMDS_H
#define ICNA3306_INIT_CMDS_H

#include "esp_lcd_icna3306.h"

#ifdef __cplusplus
extern "C" {
#endif

static const icna3306_lcd_init_cmd_t lcd_init_cmds[] = {
      // {0xFE, (uint8_t []){0x20}, 1, 10},
      // {0xF4, (uint8_t []){0x5A}, 1, 10},
      // {0xF5, (uint8_t []){0x59}, 1, 10},
      // {0xFE, (uint8_t []){0xD0}, 1, 10},
      // {0x4E, (uint8_t []){0x80}, 1, 10},
      // {0x4D, (uint8_t []){0x1F}, 1, 10},
      // {0xFE, (uint8_t []){0x40}, 1, 10},
      // {0x54, (uint8_t []){0xAF}, 1, 10},

      {0xFE, (uint8_t []){0x00}, 1, 10},
      {0xC4, (uint8_t []){0x80}, 1, 10}, // SPI
      {0x3A, (uint8_t []){0x55}, 1, 10}, // RGB565
      {0x35, (uint8_t []){0x00}, 1, 10},
      {0x53, (uint8_t []){0x20}, 1, 10},
      {0x51, (uint8_t []){0xFF}, 1, 10},
      {0x63, (uint8_t []){0xFF}, 1, 10},
      {0x2A, (uint8_t []){0x00, 0x02, 0x00, 0x7F}, 4, 10}, // 列 2~127
      {0x2B, (uint8_t []){0x00, 0x00, 0x01, 0x25}, 4, 10}, // 行 0~293
      {0x11, NULL, 0, 60},
      {0x29, NULL, 0, 0},
};

static const size_t lcd_init_cmds_size = sizeof(lcd_init_cmds) / sizeof(icna3306_lcd_init_cmd_t);

#ifdef __cplusplus
}
#endif

#endif // ICNA3306_INIT_CMDS_H
