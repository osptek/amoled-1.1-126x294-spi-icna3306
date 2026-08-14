#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io_interface.h"
#include "esp_log.h"

#include "icna3306_3wire_io.h"

#define ICNA3306_3WIRE_RAW_CHUNK_SIZE 4096
#define ICNA3306_3WIRE_PACKED_SIZE ((ICNA3306_3WIRE_RAW_CHUNK_SIZE * 9 + 7) / 8)
#define ICNA3306_3WIRE_MAX_READ_SIZE 64

static const char *TAG = "icna3306_3wire";

typedef struct {
    esp_lcd_panel_io_t base;
    spi_device_handle_t spi;
    uint8_t *packed;
    uint8_t *read_buffer;
    esp_lcd_panel_io_color_trans_done_cb_t on_color_trans_done;
    void *user_ctx;
} icna3306_3wire_io_t;

static size_t pack_9bit_word(uint8_t value, bool is_data, uint8_t *output, size_t bit_offset)
{
    uint16_t word = ((uint16_t)is_data << 8) | value;
    for (int bit = 8; bit >= 0; bit--) {
        if (word & BIT(bit)) {
            output[bit_offset >> 3] |= BIT(7 - (bit_offset & 7));
        }
        bit_offset++;
    }
    return bit_offset;
}

static size_t pack_9bit_bytes(const uint8_t *input, size_t input_size, bool is_data,
                              uint8_t *output, size_t bit_offset)
{
    for (size_t index = 0; index < input_size; index++) {
        bit_offset = pack_9bit_word(input[index], is_data, output, bit_offset);
    }
    return bit_offset;
}

static esp_err_t transmit_bits(icna3306_3wire_io_t *io, size_t bit_count, bool keep_cs_active)
{
    spi_transaction_t transaction = {
        .length = bit_count,
        .tx_buffer = io->packed,
        .flags = keep_cs_active ? SPI_TRANS_CS_KEEP_ACTIVE : 0,
    };
    return spi_device_polling_transmit(io->spi, &transaction);
}

static esp_err_t panel_io_3wire_tx_param(esp_lcd_panel_io_t *base, int lcd_cmd,
                                         const void *param, size_t param_size)
{
    icna3306_3wire_io_t *io = __containerof(base, icna3306_3wire_io_t, base);
    ESP_RETURN_ON_FALSE(lcd_cmd >= 0, ESP_ERR_INVALID_ARG, TAG, "command is required");
    ESP_RETURN_ON_FALSE(param_size <= ICNA3306_3WIRE_RAW_CHUNK_SIZE - 1,
                        ESP_ERR_INVALID_SIZE, TAG, "parameter block is too large");

    memset(io->packed, 0, ICNA3306_3WIRE_PACKED_SIZE);
    size_t bit_count = pack_9bit_word((uint8_t)lcd_cmd, false, io->packed, 0);
    if (param && param_size) {
        bit_count = pack_9bit_bytes(param, param_size, true, io->packed, bit_count);
    }

    ESP_RETURN_ON_ERROR(spi_device_acquire_bus(io->spi, portMAX_DELAY), TAG, "acquire SPI bus failed");
    esp_err_t result = transmit_bits(io, bit_count, false);
    spi_device_release_bus(io->spi);
    return result;
}

static esp_err_t panel_io_3wire_rx_param(esp_lcd_panel_io_t *base, int lcd_cmd,
                                         void *param, size_t param_size)
{
    icna3306_3wire_io_t *io = __containerof(base, icna3306_3wire_io_t, base);
    ESP_RETURN_ON_FALSE(lcd_cmd >= 0 && param && param_size, ESP_ERR_INVALID_ARG,
                        TAG, "invalid read transaction");
    ESP_RETURN_ON_FALSE(param_size <= ICNA3306_3WIRE_MAX_READ_SIZE,
                        ESP_ERR_INVALID_SIZE, TAG, "read block is too large");

    memset(io->packed, 0, ICNA3306_3WIRE_PACKED_SIZE);
    memset(io->read_buffer, 0, ICNA3306_3WIRE_MAX_READ_SIZE);
    size_t command_bits = pack_9bit_word((uint8_t)lcd_cmd, false, io->packed, 0);
    spi_transaction_t transaction = {
        .length = command_bits,
        .rxlength = param_size * 8,
        .tx_buffer = io->packed,
        .rx_buffer = io->read_buffer,
    };

    ESP_RETURN_ON_ERROR(spi_device_acquire_bus(io->spi, portMAX_DELAY), TAG, "acquire SPI bus failed");
    esp_err_t result = spi_device_polling_transmit(io->spi, &transaction);
    spi_device_release_bus(io->spi);
    if (result == ESP_OK) {
        memcpy(param, io->read_buffer, param_size);
    }
    return result;
}

static esp_err_t panel_io_3wire_tx_color(esp_lcd_panel_io_t *base, int lcd_cmd,
                                         const void *color, size_t color_size)
{
    icna3306_3wire_io_t *io = __containerof(base, icna3306_3wire_io_t, base);
    ESP_RETURN_ON_FALSE(lcd_cmd >= 0 && color && color_size, ESP_ERR_INVALID_ARG,
                        TAG, "invalid color transaction");

    esp_err_t result = spi_device_acquire_bus(io->spi, portMAX_DELAY);
    ESP_RETURN_ON_ERROR(result, TAG, "acquire SPI bus failed");

    memset(io->packed, 0, ICNA3306_3WIRE_PACKED_SIZE);
    size_t bit_count = pack_9bit_word((uint8_t)lcd_cmd, false, io->packed, 0);
    result = transmit_bits(io, bit_count, true);

    const uint8_t *source = color;
    size_t remaining = color_size;
    while (result == ESP_OK && remaining > 0) {
        size_t chunk_size = remaining > ICNA3306_3WIRE_RAW_CHUNK_SIZE ?
                            ICNA3306_3WIRE_RAW_CHUNK_SIZE : remaining;
        memset(io->packed, 0, ICNA3306_3WIRE_PACKED_SIZE);
        bit_count = pack_9bit_bytes(source, chunk_size, true, io->packed, 0);
        remaining -= chunk_size;
        result = transmit_bits(io, bit_count, remaining > 0);
        source += chunk_size;
    }

    spi_device_release_bus(io->spi);

    if (result == ESP_OK && io->on_color_trans_done) {
        esp_lcd_panel_io_event_data_t event_data = {};
        io->on_color_trans_done(&io->base, &event_data, io->user_ctx);
    }
    return result;
}

static esp_err_t panel_io_3wire_register_callbacks(esp_lcd_panel_io_t *base,
                                                   const esp_lcd_panel_io_callbacks_t *callbacks,
                                                   void *user_ctx)
{
    icna3306_3wire_io_t *io = __containerof(base, icna3306_3wire_io_t, base);
    io->on_color_trans_done = callbacks ? callbacks->on_color_trans_done : NULL;
    io->user_ctx = user_ctx;
    return ESP_OK;
}

static esp_err_t panel_io_3wire_del(esp_lcd_panel_io_t *base)
{
    icna3306_3wire_io_t *io = __containerof(base, icna3306_3wire_io_t, base);
    esp_err_t result = spi_bus_remove_device(io->spi);
    free(io->read_buffer);
    free(io->packed);
    free(io);
    return result;
}

esp_err_t icna3306_new_panel_io_3wire(spi_host_device_t host,
                                      int cs_gpio_num,
                                      int pclk_hz,
                                      esp_lcd_panel_io_handle_t *ret_io)
{
    ESP_RETURN_ON_FALSE(ret_io, ESP_ERR_INVALID_ARG, TAG, "ret_io is required");

    icna3306_3wire_io_t *io = calloc(1, sizeof(icna3306_3wire_io_t));
    ESP_RETURN_ON_FALSE(io, ESP_ERR_NO_MEM, TAG, "allocate panel IO failed");

    io->packed = heap_caps_malloc(ICNA3306_3WIRE_PACKED_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    io->read_buffer = heap_caps_malloc(ICNA3306_3WIRE_MAX_READ_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!io->packed || !io->read_buffer) {
        free(io->read_buffer);
        free(io->packed);
        free(io);
        return ESP_ERR_NO_MEM;
    }

    spi_device_interface_config_t device_config = {
        .clock_speed_hz = pclk_hz,
        .mode = 0,
        .spics_io_num = cs_gpio_num,
        .queue_size = 1,
        .flags = SPI_DEVICE_HALFDUPLEX | SPI_DEVICE_3WIRE,
    };
    esp_err_t result = spi_bus_add_device(host, &device_config, &io->spi);
    if (result != ESP_OK) {
        free(io->read_buffer);
        free(io->packed);
        free(io);
        return result;
    }

    io->base.rx_param = panel_io_3wire_rx_param;
    io->base.tx_param = panel_io_3wire_tx_param;
    io->base.tx_color = panel_io_3wire_tx_color;
    io->base.register_event_callbacks = panel_io_3wire_register_callbacks;
    io->base.del = panel_io_3wire_del;

    *ret_io = &io->base;
    ESP_LOGI(TAG, "ICNA3306 true 3-wire SPI IO created: 9-bit D/C+payload, SDA on MOSI");
    return ESP_OK;
}
