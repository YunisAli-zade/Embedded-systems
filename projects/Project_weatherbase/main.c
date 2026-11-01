// ESP32-S3 + HC-SR04 -> Wi-Fi -> HTTP POST (JSON) -> server -> SQLite
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

static const char *TAG = "app";
static EventGroupHandle_t s_wifi_events;
#define WIFI_CONNECTED_BIT BIT0

/* ---------- Wi-Fi ---------- */
static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        ESP_LOGW(TAG, "Wi-Fi qopdu, yenidən qoşulur...");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "IP alındı: " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init(void)
{
    s_wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));

    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, CONFIG_WIFI_SSID, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, CONFIG_WIFI_PASSWORD, sizeof(wc.sta.password) - 1);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
}

/* ---------- HC-SR04 ---------- */
static void sensor_init(void)
{
    gpio_config_t out = {
        .pin_bit_mask = 1ULL << CONFIG_TRIG_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out);
    gpio_config_t in = {
        .pin_bit_mask = 1ULL << CONFIG_ECHO_GPIO,
        .mode = GPIO_MODE_INPUT,
    };
    gpio_config(&in);
}

// Məsafə sm ilə; xəta olarsa -1
static float read_distance_cm(void)
{
    gpio_set_level(CONFIG_TRIG_GPIO, 0);
    esp_rom_delay_us(2);
    gpio_set_level(CONFIG_TRIG_GPIO, 1);
    esp_rom_delay_us(10);
    gpio_set_level(CONFIG_TRIG_GPIO, 0);

    int64_t t0 = esp_timer_get_time();
    while (gpio_get_level(CONFIG_ECHO_GPIO) == 0) {
        if (esp_timer_get_time() - t0 > 30000) return -1;   // echo başlamadı
    }
    int64_t start = esp_timer_get_time();
    while (gpio_get_level(CONFIG_ECHO_GPIO) == 1) {
        if (esp_timer_get_time() - start > 30000) return -1; // çox uzaq / xəta
    }
    int64_t dur_us = esp_timer_get_time() - start;
    return (float)dur_us * 0.0343f / 2.0f;
}

/* ---------- HTTP POST ---------- */
static esp_err_t post_reading(float dist)
{
    char body[128];
    snprintf(body, sizeof(body),
             "{\"device\":\"%s\",\"distance_cm\":%.2f}", CONFIG_DEVICE_ID, dist);

    esp_http_client_config_t cfg = {
        .url = CONFIG_SERVER_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 5000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "POST %s -> status %d", body, esp_http_client_get_status_code(client));
    } else {
        ESP_LOGE(TAG, "POST xətası: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
    return err;
}

/* ---------- main ---------- */
void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    sensor_init();
    wifi_init();
    xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

    while (1) {
        float d = read_distance_cm();
        if (d > 0) {
            post_reading(d);
        } else {
            ESP_LOGW(TAG, "Sensor oxuma xətası");
        }
        vTaskDelay(pdMS_TO_TICKS(CONFIG_SEND_INTERVAL_MS));
    }
}
