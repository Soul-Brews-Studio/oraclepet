/*
 * display_st7796.c — sc01 board: the display.h API on the WT32-SC01 Plus's 320x480 ST7796UI,
 * an 8-bit Intel-8080 parallel panel driven by the S3's LCD_CAM peripheral (esp_lcd i80).
 *
 * Same contract as display_ili9341.c: one RGB565 framebuffer in PSRAM (big-endian pixels,
 * the order the panel takes them on an 8-bit bus), pushed by display_flush() through two
 * bounce buffers in internal DMA RAM, one filling while the other is on the wire. The panel
 * driver is IDF's built-in ST7789 one: its init (SWRESET, SLPOUT, MADCTL, COLMOD 16-bit,
 * DISPON) is what the ST7796 takes too. Settings follow ESPHome's WT32-SC01-PLUS model and
 * the LovyanGFX config in lab/buddy-port: BGR order, colour inversion on, mirror X.
 */
#include "display.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/ledc.h"
#include "esp_heap_caps.h"
#include "esp_lcd_io_i80.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"

static const char *TAG = "display";

#define LCD_PCLK_HZ  (20 * 1000 * 1000)   /* LovyanGFX runs this panel at 40 MHz; 20 is the safe start */
#define BAND_ROWS    20
#define BAND_BYTES   (DISP_W * BAND_ROWS * 2)
#define FB_BYTES     (DISP_W * DISP_H * 2)

static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_fb;
static uint16_t *s_band[2];
static SemaphoreHandle_t s_free;          /* counts bounce buffers not on the wire */

static bool on_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *e, void *ctx)
{
    BaseType_t woke = pdFALSE;
    xSemaphoreGiveFromISR(s_free, &woke);
    return woke == pdTRUE;
}

static void backlight_init(void)
{
    ledc_timer_config_t t = { .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_8_BIT,
                              .timer_num = LEDC_TIMER_0, .freq_hz = 25000, .clk_cfg = LEDC_AUTO_CLK };
    ledc_timer_config(&t);
    ledc_channel_config_t c = { .gpio_num = DISP_PIN_BL, .speed_mode = LEDC_LOW_SPEED_MODE,
                                .channel = LEDC_CHANNEL_0, .timer_sel = LEDC_TIMER_0, .duty = 0 };
    ledc_channel_config(&c);
}

void display_set_backlight(uint8_t level)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, level);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

esp_err_t display_init(void)
{
    backlight_init();

    esp_lcd_i80_bus_handle_t bus = NULL;
    esp_lcd_i80_bus_config_t bus_cfg = {
        .dc_gpio_num = DISP_PIN_DC, .wr_gpio_num = DISP_PIN_WR, .clk_src = LCD_CLK_SRC_DEFAULT,
        .data_gpio_nums = { DISP_PIN_D0, DISP_PIN_D1, DISP_PIN_D2, DISP_PIN_D3,
                            DISP_PIN_D4, DISP_PIN_D5, DISP_PIN_D6, DISP_PIN_D7 },
        .bus_width = 8, .max_transfer_bytes = BAND_BYTES, .dma_burst_size = 64,
    };
    ESP_ERROR_CHECK(esp_lcd_new_i80_bus(&bus_cfg, &bus));

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_i80_config_t io_cfg = {
        .cs_gpio_num = -1,                       /* CS is tied active on this board */
        .pclk_hz = LCD_PCLK_HZ, .trans_queue_depth = 4, .on_color_trans_done = on_done,
        .lcd_cmd_bits = 8, .lcd_param_bits = 8,
        .dc_levels = { .dc_idle_level = 0, .dc_cmd_level = 0, .dc_dummy_level = 0, .dc_data_level = 1 },
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i80(bus, &io_cfg, &io));

    esp_lcd_panel_dev_config_t pcfg = {
        .reset_gpio_num = DISP_PIN_RST, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR, .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &pcfg, &s_panel));
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_invert_color(s_panel, true);
    esp_lcd_panel_swap_xy(s_panel, false);          /* portrait, 320 wide */
    esp_lcd_panel_mirror(s_panel, BOARD_LCD_MIRROR_X, BOARD_LCD_MIRROR_Y);   /* board.h */

    s_fb = heap_caps_aligned_alloc(64, FB_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    for (int i = 0; i < 2; i++) s_band[i] = heap_caps_malloc(BAND_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_free = xSemaphoreCreateCounting(2, 2);
    if (!s_fb || !s_band[0] || !s_band[1] || !s_free) {
        ESP_LOGE(TAG, "out of memory for the framebuffer / bounce buffers");
        return ESP_ERR_NO_MEM;
    }
    memset(s_fb, 0, FB_BYTES);
    display_flush();
    esp_lcd_panel_disp_on_off(s_panel, true);
    display_set_backlight(255);
    ESP_LOGI(TAG, "ST7796 %dx%d on i80 8-bit @ %d MHz, fb %d KB in PSRAM, 2 x %d KB bounce",
             DISP_W, DISP_H, LCD_PCLK_HZ / 1000000, FB_BYTES / 1024, BAND_BYTES / 1024);
    return ESP_OK;
}

esp_lcd_panel_handle_t display_panel(void) { return s_panel; }
uint16_t *display_framebuffer(void) { return s_fb; }

esp_err_t display_flush(void) { return display_flush_rows(0, DISP_H); }

esp_err_t display_flush_rows(int y0, int nrows)
{
    if (!s_panel || !s_fb) return ESP_ERR_INVALID_STATE;
    if (y0 < 0) { nrows += y0; y0 = 0; }
    int end = y0 + nrows > DISP_H ? DISP_H : y0 + nrows;
    static int i;                                          /* alternate the bounce buffers across calls */
    for (int y = y0; y < end; y += BAND_ROWS, i ^= 1) {
        int rows = end - y < BAND_ROWS ? end - y : BAND_ROWS;
        xSemaphoreTake(s_free, portMAX_DELAY);            /* this bounce buffer is off the wire */
        memcpy(s_band[i], s_fb + y * DISP_W, (size_t)rows * DISP_W * 2);
        esp_err_t e = esp_lcd_panel_draw_bitmap(s_panel, 0, y, DISP_W, y + rows, s_band[i]);
        if (e != ESP_OK) { xSemaphoreGive(s_free); return e; }
    }
    return ESP_OK;
}

void display_fb_clear(uint16_t rgb565)
{
    if (!s_fb) return;
    for (size_t i = 0; i < (size_t)DISP_W * DISP_H; i++) s_fb[i] = rgb565;
}

esp_err_t display_fill_solid(uint16_t rgb565)
{
    display_fb_clear(rgb565);
    return display_flush();
}

esp_err_t display_test_pattern(void)
{
    if (!s_fb) return ESP_ERR_INVALID_STATE;
    static const uint16_t bars[] = { 0x00F8, 0xE007, 0x1F00, 0xFFFF };   /* R G B W, big-endian */
    for (int y = 0; y < DISP_H; y++)
        for (int x = 0; x < DISP_W; x++) s_fb[y * DISP_W + x] = bars[x * 4 / DISP_W];
    return display_flush();
}
