#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_log.h"

static const char *TAG = "ESP_NOW_RECEIVER";

typedef struct {
    int counter;
    char message[32];
} data_packet_t;

void recv_callback(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    data_packet_t *packet = (data_packet_t *)data;
    ESP_LOGI(TAG, "Alındı: counter=%d, mesaj=%s", packet->counter, packet->message);
}

void app_main(void) {
    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();

    esp_now_init();
    esp_now_register_recv_cb(recv_callback);

    ESP_LOGI(TAG, "Qəbul üçün hazırdır...");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}