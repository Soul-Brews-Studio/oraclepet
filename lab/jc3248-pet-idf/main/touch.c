/*
 * jc3248-pet-idf — STEP 4: AXS15231 capacitive touch (INT-gated).
 *
 * Built on the managed component's touch driver (esp_lcd_touch_axs15231b, pulled
 * transitively by esp_lcd_axs15231b). New i2c_master bus on SDA4/SCL8, addr 0x3B,
 * INT on GPIO3 (NEGEDGE). The component's interrupt_callback latches a flag; we
 * read I2C ONLY when it's set -> no phantom taps at rest (the Arduino-body bug).
 */
#include "touch.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_axs15231b.h"

static const char *TAG = "touch";

#define T_SDA 4
#define T_SCL 8
#define T_INT 3
#define T_W   320
#define T_H   480

static esp_lcd_touch_handle_t s_tp = NULL;
static esp_lcd_panel_io_handle_t s_io = NULL;
static volatile bool s_irq = false;   /* set by the INT edge callback */

/* Called by the esp_lcd_touch ISR on a GPIO3 NEGEDGE — just latch it. */
static void touch_int_cb(esp_lcd_touch_handle_t tp)
{
    (void)tp;
    s_irq = true;
}

esp_err_t touch_init(void)
{
    /* 1) i2c_master bus (IDF v6 driver) on SDA4/SCL8, internal pull-ups. */
    i2c_master_bus_handle_t bus = NULL;
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,                       /* auto-select a free port */
        .sda_io_num = T_SDA,
        .scl_io_num = T_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &bus), TAG, "i2c_new_master_bus");

    /* 2) panel-IO for the touch controller (0x3B, 400kHz). */
    esp_lcd_panel_io_handle_t io = NULL;
    /* The component's CONFIG_EX(scl_speed_hz) macro names its parameter the SAME
     * as the field (.scl_speed_hz = scl_speed_hz), so a literal expands to
     * ".400000 = 400000" and won't compile. Use plain CONFIG() + set speed here. */
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_AXS15231B_CONFIG();
    io_cfg.scl_speed_hz = 400000;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(bus, &io_cfg, &io), TAG, "new_panel_io_i2c");
    s_io = io;                                /* touch_poll_ev reads the raw report itself */

    /* 3) the AXS15231 touch driver, INT on GPIO3 (active-low), our int callback. */
    esp_lcd_touch_config_t tp_cfg = {
        .x_max = T_W,
        .y_max = T_H,
        .rst_gpio_num = -1,                   /* no touch RST pin -> SWRESET */
        .int_gpio_num = T_INT,
        .levels = { .reset = 0, .interrupt = 0 },   /* INT active low (NEGEDGE) */
        .flags = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
        .interrupt_callback = touch_int_cb,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_axs15231b(io, &tp_cfg, &s_tp), TAG, "new_axs15231b");

    ESP_LOGI(TAG, "touch ready — AXS15231 0x3B (sda%d/scl%d), INT gpio%d, edge-gated (no phantom)",
             T_SDA, T_SCL, T_INT);
    return ESP_OK;
}

/* Raw AXS15231B report, including the event bits the managed driver drops:
 * byte0 gesture, byte1 points, byte2 [7:6] event | x[11:8], byte3 x[7:0], byte4 y[11:8], byte5 y[7:0].
 * Event (FocalTech-style): 0 finger down, 1 finger up, 2 contact, 3 none. One press sends a
 * down AND an up report (measured 140-240 ms apart), which the driver returns as two touches. */
bool touch_poll_ev(uint16_t *x, uint16_t *y, int *event)
{
    if (s_io == NULL || !s_irq) return false;
    s_irq = false;
    static const uint8_t cmd[11] = {0xb5, 0xab, 0xa5, 0x5a, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00};
    uint8_t d[8] = {0};
    if (esp_lcd_panel_io_tx_param(s_io, -1, cmd, sizeof cmd) != ESP_OK) return false;
    if (esp_lcd_panel_io_rx_param(s_io, -1, d, sizeof d) != ESP_OK) return false;
    if (d[1] == 0 || d[1] > 2) return false;               /* no point / garbage */
    if (x) *x = (uint16_t)(((d[2] & 0x0F) << 8) | d[3]);
    if (y) *y = (uint16_t)(((d[4] & 0x0F) << 8) | d[5]);
    if (event) *event = d[2] >> 6;
    return true;
}

bool touch_poll(uint16_t *x, uint16_t *y)
{
    if (s_tp == NULL || !s_irq) return false;   /* INT gate: read only on a real edge */
    s_irq = false;

    if (esp_lcd_touch_read_data(s_tp) != ESP_OK) return false;

    esp_lcd_touch_point_data_t pts[1];
    uint8_t cnt = 0;
    if (esp_lcd_touch_get_data(s_tp, pts, &cnt, 1) != ESP_OK) return false;
    if (cnt < 1) return false;

    if (x) *x = pts[0].x;
    if (y) *y = pts[0].y;
    return true;
}
