idf_component_register(SRCS "esp_lcd_icna3306.c" "esp_lcd_icna3306_spi.c" "esp_lcd_icna3306_mipi.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "priv_include"
                       REQUIRES "esp_lcd" "driver")
