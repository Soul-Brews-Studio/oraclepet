/*
 * mic.c — the 2.8in pet board's ES8311 as an esp_codec_dev handle, 16 kHz mono 16-bit.
 * The pet firmware's bring-up (lab/jc3248-pet-idf/main/audio.c, after xiaozhi's
 * Es8311AudioCodec on this exact board): duplex I2S0, the codec held in reset before init,
 * MCLK 256 x fs, mic gain 30 dB. The speaker amp (GPIO1, active low) stays off.
 */
#include "mic.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "mic";

#define SR        16000
#define PIN_MCLK  GPIO_NUM_4
#define PIN_BCK   GPIO_NUM_5
#define PIN_WS    GPIO_NUM_7
#define PIN_DOUT  GPIO_NUM_8
#define PIN_DIN   GPIO_NUM_6
#define PIN_PA    GPIO_NUM_1
#define I2C_SDA   16
#define I2C_SCL   15

esp_codec_dev_handle_t mic_open(void)
{
    static esp_codec_dev_handle_t dev;
    if (dev) return dev;
    i2c_master_bus_handle_t bus = NULL;
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0, .sda_io_num = I2C_SDA, .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK) { ESP_LOGE(TAG, "i2c bus failed"); return NULL; }

    i2s_chan_handle_t tx = NULL, rx = NULL;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear_after_cb = true;
    if (i2s_new_channel(&chan_cfg, &tx, &rx) != ESP_OK) { ESP_LOGE(TAG, "i2s channel failed"); return NULL; }
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SR),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = { .mclk = PIN_MCLK, .bclk = PIN_BCK, .ws = PIN_WS, .dout = PIN_DOUT, .din = PIN_DIN },
    };
    if (i2s_channel_init_std_mode(tx, &std_cfg) != ESP_OK || i2s_channel_init_std_mode(rx, &std_cfg) != ESP_OK ||
        i2s_channel_enable(tx) != ESP_OK || i2s_channel_enable(rx) != ESP_OK) {
        ESP_LOGE(TAG, "i2s init failed");
        return NULL;
    }

    /* esp_codec_dev 2.x (av_render needs it): ctrl over the I2C bus, data over the I2S RX channel */
    audio_codec_i2c_cfg_t i2c_cfg = { .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = bus };
    const audio_codec_ctrl_if_t *ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg);
    const audio_codec_gpio_if_t *gpio = audio_codec_new_gpio();
    audio_codec_i2s_cfg_t i2s_cfg = { .port = I2S_NUM_0, .rx_handle = rx, .tx_handle = NULL };
    const audio_codec_data_if_t *data = audio_codec_new_i2s_data(&i2s_cfg);
    if (!ctrl || !gpio || !data) { ESP_LOGE(TAG, "codec interfaces failed"); return NULL; }
    uint8_t v = 0x1F;                              /* hold the digital blocks in reset first */
    ctrl->write_reg(ctrl, 0x00, 1, &v, 1);
    vTaskDelay(pdMS_TO_TICKS(5));
    es8311_codec_cfg_t es = {
        .ctrl_if = ctrl,
        .gpio_if = gpio,
        .sys_cfg = { .is_master = false, .no_mclk = false },
        .pa_cfg = { .pa_pin = PIN_PA, .pa_active_low = true,
                    .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 } },
    };
    const audio_codec_if_t *codec = es8311_codec_new(&es);
    esp_codec_dev_cfg_t dev_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = codec, .data_if = data };
    dev = codec ? esp_codec_dev_new(&dev_cfg) : NULL;
    if (!dev) { ESP_LOGE(TAG, "ES8311 failed"); return NULL; }
    /* esp_capture opens the device itself at the rate the peer asks for */
    esp_codec_dev_set_in_gain(dev, 30.0f);
    gpio_set_level(PIN_PA, 1);                     /* amp off (active low): no hiss next to the mic */
    ESP_LOGI(TAG, "ES8311 ready (mic gain 30 dB)");
    return dev;
}
