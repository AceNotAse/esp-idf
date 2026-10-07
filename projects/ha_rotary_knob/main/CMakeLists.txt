idf_component_register(SRCS "main.c" "board.c" "encoder.c" "ha_client.c" "ui.c"
                       PRIV_REQUIRES esp_driver_gpio esp_driver_spi esp_driver_i2c esp_driver_pcnt
                                     esp_driver_ledc esp_lcd esp_wifi esp_netif esp_event nvs_flash
                                     esp_http_client esp-tls esp_timer
                       INCLUDE_DIRS ".")
