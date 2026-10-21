idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_wifi esp_event esp_netif esp_http_client nvs_flash esp_timer esp_rom esp_driver_gpio
)