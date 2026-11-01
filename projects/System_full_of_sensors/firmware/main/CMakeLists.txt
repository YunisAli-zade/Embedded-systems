idf_component_register(
    SRCS "main.c" "wifi.c" "sensors.c" "uploader.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_event esp_netif nvs_flash esp_http_client
             esp_adc esp_timer esp_rom esp_driver_gpio esp_driver_i2c esp_driver_tsens
             freertos log
)
