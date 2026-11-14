/*
 * touch_ft6336.c — lcd28 board: the touch.h API on an FT6336 (I2C 0x38, shared bus).
 *
 * No INT pin is wired up here (xiaozhi polls this panel too), so touch_poll() reads
 * the point count every 30 ms and reports one event per press (on touch-down),
 * which is what the JC3248's INT-gated driver delivers as well.
 */
#include "touch.h"

#include "board_i2c.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "touch";
static i2c_master_dev_handle_t s_dev;
static bool s_down;
static int64_t s_next_us;

esp_err_t touch_init(void)
{
    i2c_master_bus_handle_t bus = board_i2c_bus();
    if (!bus) return ESP_FAIL;
    i2c_device_config_t cfg = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = 0x38, .scl_speed_hz = 400000 };
    esp_err_t e = i2c_master_bus_add_device(bus, &cfg, &s_dev);
    if (e == ESP_OK) ESP_LOGI(TAG, "FT6336 at 0x38 (sda%d/scl%d), polled", BOARD_I2C_SDA, BOARD_I2C_SCL);
    return e;
}

bool touch_poll(uint16_t *x, uint16_t *y)
{
    if (!s_dev) return false;
    int64_t now = esp_timer_get_time();
    if (now < s_next_us) return false;
    s_next_us = now + 30000;
    uint8_t reg = 0x02, b[5];
    if (i2c_master_transmit_receive(s_dev, &reg, 1, b, sizeof b, 20) != ESP_OK) return false;
    bool touched = (b[0] & 0x0F) > 0;
    bool press = touched && !s_down;               /* report the touch-down only */
    s_down = touched;
    if (!press) return false;
    if (x) *x = ((b[1] & 0x0F) << 8) | b[2];
    if (y) *y = ((b[3] & 0x0F) << 8) | b[4];
    return true;
}
