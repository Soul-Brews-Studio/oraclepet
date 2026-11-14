idf_component_register(SRCS "main.c"
                       PRIV_REQUIRES discord_voice esp_wifi esp_netif esp_event nvs_flash esp_timer
                       INCLUDE_DIRS ".")
