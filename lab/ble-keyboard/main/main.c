/*
 * BLE Keyboard → JC3248W535 Display
 * Scans for BLE HID keyboards, connects, shows keystrokes on the 320×480 screen.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_hidh.h"
#include "esp_hid_gap.h"
#include "display.h"
#include "hud_gfx.h"
#include "esp_hidh_gattc.h"

static const char *TAG = "BLE_KB";

/* ── HID keycode → ASCII (US layout, basic) ── */
static const char KC_TO_ASCII[128] = {
    [0x04] = 'a', [0x05] = 'b', [0x06] = 'c', [0x07] = 'd', [0x08] = 'e',
    [0x09] = 'f', [0x0A] = 'g', [0x0B] = 'h', [0x0C] = 'i', [0x0D] = 'j',
    [0x0E] = 'k', [0x0F] = 'l', [0x10] = 'm', [0x11] = 'n', [0x12] = 'o',
    [0x13] = 'p', [0x14] = 'q', [0x15] = 'r', [0x16] = 's', [0x17] = 't',
    [0x18] = 'u', [0x19] = 'v', [0x1A] = 'w', [0x1B] = 'x', [0x1C] = 'y',
    [0x1D] = 'z',
    [0x1E] = '1', [0x1F] = '2', [0x20] = '3', [0x21] = '4', [0x22] = '5',
    [0x23] = '6', [0x24] = '7', [0x25] = '8', [0x26] = '9', [0x27] = '0',
    [0x28] = '\n', [0x2C] = ' ', [0x2D] = '-', [0x2E] = '=',
    [0x2F] = '[', [0x30] = ']', [0x33] = ';', [0x34] = '\'',
    [0x36] = ',', [0x37] = '.', [0x38] = '/',
};

/* ── Screen text buffer (simple terminal) ── */
#define TERM_COLS 26
#define TERM_ROWS 20
static char term_buf[TERM_ROWS][TERM_COLS + 1];
static int term_row = 0, term_col = 0;
static bool display_ready = false;

static void term_clear(void) {
    memset(term_buf, 0, sizeof(term_buf));
    term_row = 0;
    term_col = 0;
}

static void term_putchar(char c) {
    if (c == '\n' || term_col >= TERM_COLS) {
        term_col = 0;
        term_row++;
        if (term_row >= TERM_ROWS) {
            memmove(term_buf[0], term_buf[1], (TERM_ROWS - 1) * (TERM_COLS + 1));
            memset(term_buf[TERM_ROWS - 1], 0, TERM_COLS + 1);
            term_row = TERM_ROWS - 1;
        }
        if (c == '\n') return;
    }
    term_buf[term_row][term_col++] = c;
}

static void term_render(void) {
    if (!display_ready) return;
    uint16_t *fb = display_framebuffer();
    if (!fb) return;
    /* clear to black */
    gfx_fill_rect(fb, DISP_W, DISP_H, 0, 0, DISP_W, DISP_H, GFX_BLACK);
    /* title bar */
    gfx_text(fb, DISP_W, DISP_H, 8, 4, "BLE Keyboard", 3, GFX_YELLOW, GFX_TRANSPARENT);
    gfx_text(fb, DISP_W, DISP_H, 8, 32, "type on your BT keyboard", 1, GFX_GREY, GFX_TRANSPARENT);
    /* render terminal buffer */
    for (int r = 0; r < TERM_ROWS; r++) {
        if (term_buf[r][0]) {
            gfx_text(fb, DISP_W, DISP_H, 8, 52 + r * 20, term_buf[r], 2, GFX_WHITE, GFX_TRANSPARENT);
        }
    }
    display_flush();
}

/* ── HID Host callback ── */
static void hidh_callback(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    esp_hidh_event_t event = (esp_hidh_event_t)id;
    esp_hidh_event_data_t *param = (esp_hidh_event_data_t *)event_data;

    switch (event) {
    case ESP_HIDH_OPEN_EVENT:
        if (param->open.status == ESP_OK) {
            const char *name = esp_hidh_dev_name_get(param->open.dev);
            ESP_LOGI(TAG, "CONNECTED: %s", name ? name : "unknown");
            term_clear();
            char msg[40];
            snprintf(msg, sizeof(msg), "Connected: %s", name ? name : "?");
            term_putchar('\n');
            for (const char *p = msg; *p; p++) term_putchar(*p);
            term_putchar('\n');
            term_render();
        }
        break;

    case ESP_HIDH_INPUT_EVENT:
        if (param->input.usage == ESP_HID_USAGE_KEYBOARD && param->input.length >= 3) {
            uint8_t mod = param->input.data[0];
            bool shift = (mod & 0x22); /* left or right shift */
            for (int i = 2; i < param->input.length && i < 8; i++) {
                uint8_t kc = param->input.data[i];
                if (kc == 0) continue;
                if (kc < 128 && KC_TO_ASCII[kc]) {
                    char c = KC_TO_ASCII[kc];
                    if (shift && c >= 'a' && c <= 'z') c -= 32;
                    term_putchar(c);
                    ESP_LOGI(TAG, "KEY: '%c' (0x%02x mod=0x%02x)", c, kc, mod);
                } else if (kc == 0x29) { /* ESC */
                    term_clear();
                } else if (kc == 0x2A) { /* Backspace */
                    if (term_col > 0) {
                        term_col--;
                        term_buf[term_row][term_col] = 0;
                    }
                } else {
                    ESP_LOGI(TAG, "KEY: raw 0x%02x mod=0x%02x", kc, mod);
                }
            }
            term_render();
        }
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, param->input.data, param->input.length, ESP_LOG_DEBUG);
        break;

    case ESP_HIDH_CLOSE_EVENT:
        ESP_LOGI(TAG, "DISCONNECTED: %s", esp_hidh_dev_name_get(param->close.dev));
        term_clear();
        for (const char *p = "Disconnected. Rescanning..."; *p; p++) term_putchar(*p);
        term_render();
        break;

    default:
        ESP_LOGI(TAG, "EVENT: %d", event);
        break;
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "BLE Keyboard → Display (JC3248W535)");

    /* NVS (required for BT) */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    /* Display */
    display_init();
    display_set_backlight(200);
    display_ready = true;
    term_clear();
    for (const char *p = "Waiting for BLE keyboard..."; *p; p++) term_putchar(*p);
    term_render();

    /* BLE HID Host */
    ESP_LOGI(TAG, ">>> esp_hid_gap_init(BLE_MODE)...");
    ESP_ERROR_CHECK(esp_hid_gap_init(HIDH_BLE_MODE));
    ESP_LOGI(TAG, ">>> esp_hid_gap_init OK");

    esp_hidh_config_t config = {
        .callback = hidh_callback,
        .event_stack_size = 4096,
        .callback_arg = NULL,
    };
    ESP_LOGI(TAG, ">>> registering GATTC callback...");
    ESP_ERROR_CHECK(esp_ble_gattc_register_callback(esp_hidh_gattc_event_handler));
    ESP_LOGI(TAG, ">>> esp_hidh_init...");
    ESP_ERROR_CHECK(esp_hidh_init(&config));
    ESP_LOGI(TAG, ">>> esp_hidh_init OK — entering scan loop");

    /* Scan loop */
    while (1) {
        size_t num = 0;
        esp_hid_scan_result_t *results = NULL;
        ESP_LOGI(TAG, "Scanning for BLE HID devices (5s)...");
        esp_hid_scan(5, &num, &results);
        ESP_LOGI(TAG, "Found %d devices", (int)num);

        esp_hid_scan_result_t *r = results;
        esp_hid_scan_result_t *kbd = NULL;
        while (r) {
            ESP_LOGI(TAG, "  DEVICE: \"%s\" transport=%s usage=%s rssi=%d",
                r->name ? r->name : "(no name)",
                r->transport == ESP_HID_TRANSPORT_BLE ? "BLE" : "BT",
                esp_hid_usage_str(r->usage), r->rssi);
            if (r->transport == ESP_HID_TRANSPORT_BLE && !kbd) {
                /* Accept any BLE HID device (keyboard, mouse, generic) —
                   Keychron may advertise as GENERIC not KEYBOARD */
                kbd = r;
                ESP_LOGI(TAG, "  → will connect to: %s", r->name ? r->name : "?");
            }
            r = r->next;
        }
        if (kbd) {
            esp_hidh_dev_open(kbd->bda, kbd->transport, kbd->ble.addr_type);
        }
        esp_hid_scan_results_free(results);

        if (kbd) {
            /* Connected — wait until disconnect triggers rescan */
            vTaskDelay(pdMS_TO_TICKS(30000));
        } else {
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }
}
