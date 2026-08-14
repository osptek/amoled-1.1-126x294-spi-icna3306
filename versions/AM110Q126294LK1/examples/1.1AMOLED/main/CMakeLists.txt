idf_component_register(SRCS "main.c" "icna3306_3wire_io.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_lcd_icna3306 esp_lcd_touch_chsc6417)
