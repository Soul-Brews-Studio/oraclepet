idf_component_register(SRCS "dv_ws.c" "dv_gateway.c" "dv_voice.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES tcp_transport esp-tls mbedtls esp_timer lwip esp_event)
