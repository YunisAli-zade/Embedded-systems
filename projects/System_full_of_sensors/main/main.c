#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"

#define VIBRATOR_SENSOR GPIO_NUM_4


void app_main(void)
{
    gpio_config_t gpioconf = {
        .pin_bit_mask = (1ULL << VIBRATOR_SENSOR),
        .mode = GPIO_MODE_INPUT,
        .intr_type = GPIO_INTR_ANYEDGE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE
    };
    gpio_config(&gpioconf);
    while (1)
    {
        int result = gpio_get_level(VIBRATOR_SENSOR);
        printf("%d\n", result);
        vTaskDelay(400 / portTICK_PERIOD_MS);
    }
}