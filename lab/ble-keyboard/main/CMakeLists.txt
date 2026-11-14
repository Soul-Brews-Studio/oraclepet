idf_component_register(
    SRCS "main.c" "display.c" "hud_gfx.c" "esp_hid_gap.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_hid nvs_flash esp_lcd driver esp_timer bt esp_driver_ledc esp_driver_spi
)
