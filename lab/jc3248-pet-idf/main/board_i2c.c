/* board_i2c.c — see board_i2c.h. */
#include "board_i2c.h"
#include "esp_log.h"

static i2c_master_bus_handle_t s_bus;

i2c_master_bus_handle_t board_i2c_bus(void)
{
    if (s_bus) return s_bus;
    i2c_master_bus_config_t cfg = {
        .i2c_port = BOARD_I2C_PORT,
        .sda_io_num = BOARD_I2C_SDA,
        .scl_io_num = BOARD_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&cfg, &s_bus) != ESP_OK) {
        ESP_LOGE("board_i2c", "i2c bus on sda%d/scl%d failed", BOARD_I2C_SDA, BOARD_I2C_SCL);
        s_bus = NULL;
    }
    return s_bus;
}
