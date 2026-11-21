/*
 * display_qemu.c — qemu board: the display.h API on Espressif QEMU's virtual RGB panel
 * (managed component espressif/esp_lcd_qemu_rgb; QEMU >= 8.1.3-20231206, `-display sdl`
 * opens the window, `screendump` in the QEMU monitor saves it).
 *
 * Same contract as the real panels: one DISP_W x DISP_H RGB565 framebuffer in PSRAM holding
 * BIG-ENDIAN pixels (gif.cpp BIG_ENDIAN_PIXELS, hud_gfx byte-swaps), so the GIF, HUD, clock
 * and pet-app canvas draw into it unchanged. The virtual panel reads RGB565 in host (little-
 * endian) order from its own frame buffer at 0x20000000, so display_flush_rows() byte-swaps
 * the dirty rows into that buffer and asks the panel to repaint them.
 *
 * No backlight (display_set_backlight is a no-op), no mirror/invert: the virtual panel draws
 * the buffer as is, which is already the portrait picture the real boards show.
 */
#include "display.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_qemu_rgb.h"
#include "esp_log.h"

static const char *TAG = "display";

#define FB_BYTES (DISP_W * DISP_H * 2)

static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_fb;      /* the app's framebuffer, big-endian RGB565 (PSRAM) */
static uint16_t *s_qfb;     /* the virtual panel's own frame buffer, little-endian RGB565 */

void display_set_backlight(uint8_t level) { (void)level; }

esp_err_t display_init(void)
{
    esp_lcd_rgb_qemu_config_t cfg = { .width = DISP_W, .height = DISP_H, .bpp = RGB_QEMU_BPP_16 };
    esp_err_t e = esp_lcd_new_rgb_qemu(&cfg, &s_panel);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_rgb_qemu: %s (not running in QEMU?)", esp_err_to_name(e));
        s_panel = NULL;
        return e;
    }
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    void *qfb = NULL;
    esp_lcd_rgb_qemu_get_frame_buffer(s_panel, &qfb);
    s_qfb = qfb;

    s_fb = heap_caps_aligned_alloc(64, FB_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_fb) s_fb = heap_caps_malloc(FB_BYTES, MALLOC_CAP_8BIT);
    if (!s_fb || !s_qfb) {
        ESP_LOGE(TAG, "no framebuffer (fb %p, panel fb %p)", s_fb, s_qfb);
        return ESP_ERR_NO_MEM;
    }
    memset(s_fb, 0, FB_BYTES);
    display_flush();
    ESP_LOGI(TAG, "QEMU virtual RGB panel %dx%d RGB565, fb %d KB, panel fb at %p",
             DISP_W, DISP_H, FB_BYTES / 1024, s_qfb);
    return ESP_OK;
}

esp_lcd_panel_handle_t display_panel(void) { return s_panel; }
uint16_t *display_framebuffer(void) { return s_fb; }

esp_err_t display_flush(void) { return display_flush_rows(0, DISP_H); }

esp_err_t display_flush_rows(int y0, int nrows)
{
    if (!s_panel || !s_fb || !s_qfb) return ESP_ERR_INVALID_STATE;
    if (y0 < 0) { nrows += y0; y0 = 0; }
    int end = y0 + nrows > DISP_H ? DISP_H : y0 + nrows;
    if (end <= y0) return ESP_OK;
    const uint16_t *src = s_fb + (size_t)y0 * DISP_W;
    uint16_t *dst = s_qfb + (size_t)y0 * DISP_W;
    for (size_t i = 0, n = (size_t)(end - y0) * DISP_W; i < n; i++) dst[i] = __builtin_bswap16(src[i]);
    return esp_lcd_panel_draw_bitmap(s_panel, 0, y0, DISP_W, end, dst);
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
