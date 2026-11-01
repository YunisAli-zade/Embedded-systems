#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "sensors.h"
#include "uploader.h"
#include "wifi.h"

static const char *TAG = "main";

// Əsas dövr: ölç -> bufferə at -> serverə göndər -> gözlə
static void gateway_task(void *arg)
{
    reading_t batch[16];
    TickType_t last_wake = xTaskGetTickCount();

    while (1) {
        size_t n = sensors_read_all(batch, 16);
        for (size_t i = 0; i < n; i++) {
            ESP_LOGI(TAG, "%s = %.1f %s  [%s]", batch[i].name, batch[i].value, batch[i].unit,
                     batch[i].label[0] ? batch[i].label : "-");
        }
        uploader_push(batch, n);

        if (wifi_is_connected()) {
            uploader_flush();
        } else {
            ESP_LOGW(TAG, "WiFi not connected, readings stay in buffer");
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(CONFIG_GW_SAMPLE_PERIOD_MS));
    }
}

void app_main(void)
{
    // WiFi öz parametrlərini NVS-də saxlayır, ona görə NVS lazımdır
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    sensors_init();
    wifi_start();

    if (!wifi_wait_connected(15000)) {
        ESP_LOGW(TAG, "WiFi not ready yet (SSID/password/2.4GHz?), will keep trying in background");
    }
    ESP_LOGI(TAG, "Server: %s", CONFIG_GW_SERVER_URL);

    // 1-ci nüvədə işləsin: WiFi 0-cı nüvədədir, mikrosaniyəlik DHT11 ölçməsi az pozulsun
    xTaskCreatePinnedToCore(gateway_task, "gateway", 8192, NULL, 5, NULL, 1);
}
