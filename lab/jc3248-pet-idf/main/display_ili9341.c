/*
 * display_ili9341.c — lcd28 board: the display.h API on a 240x320 ILI9341 over SPI3.
 *
 * Same contract as display.c (JC3248): one RGB565 framebuffer in PSRAM that the GIF
 * engine, HUD and clock draw into (big-endian pixels, as the panel wants), pushed
 * whole by display_flush(). SPI DMA cannot read this PSRAM buffer directly here, so
 * the flush copies it through two 40-row bounce buffers in internal DMA RAM, one
 * filling while the other is on the wire. Settings follow xiaozhi's board file for
 * this panel: BGR order, colour inversion on, SPI mode 0.
 */
#include "display.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"
#include "esp_log.h"

static const char *TAG = "display";

#define LCD_HOST     SPI3_HOST
#define LCD_PCLK_HZ  (40 * 1000 * 1000)   /* xiaozhi runs this panel at 20 MHz; 40 halves the flush */
#define BAND_ROWS    40
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

    spi_bus_config_t bus = {
        .sclk_io_num = DISP_PIN_SCLK, .mosi_io_num = DISP_PIN_MOSI, .miso_io_num = DISP_PIN_MISO,
        .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = BAND_BYTES,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = DISP_PIN_CS, .dc_gpio_num = DISP_PIN_DC, .spi_mode = 0,
        .pclk_hz = LCD_PCLK_HZ, .trans_queue_depth = 4, .lcd_cmd_bits = 8, .lcd_param_bits = 8,
        .on_color_trans_done = on_done,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io));

    esp_lcd_panel_dev_config_t pcfg = {
        .reset_gpio_num = -1, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR, .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(io, &pcfg, &s_panel));
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_invert_color(s_panel, true);
    esp_lcd_panel_swap_xy(s_panel, false);          /* portrait, 240 wide */
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
    ESP_LOGI(TAG, "ILI9341 %dx%d on SPI3 @ %d MHz, fb %d KB in PSRAM, 2 x %d KB bounce",
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
