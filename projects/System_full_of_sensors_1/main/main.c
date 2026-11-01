/*
vcc-5v
gnd-gnd
AO-GPIO_num_36
*/

#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"

#include "esp_adc/adc_oneshot.h"


static const char *TAG = "MIC";


void app_main(void)
{
    /*
     * ADC1-i başladırıq
     */
    adc_oneshot_unit_handle_t adc_handle;

    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT_1,
    };

    ESP_ERROR_CHECK(
        adc_oneshot_new_unit(
            &init_config,
            &adc_handle
        )
    );


    /*
     * GPIO34 = ADC1_CHANNEL_6
     */
    adc_oneshot_chan_cfg_t channel_config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,

        /*
         * 12 dB attenuation
         */
        .atten = ADC_ATTEN_DB_12,
    };


    ESP_ERROR_CHECK(
        adc_oneshot_config_channel(
            adc_handle,
            ADC_CHANNEL_6,
            &channel_config
        )
    );


    printf("\n");
    printf("============================\n");
    printf("   ESP32 MICROPHONE TEST\n");
    printf("============================\n");
    printf("AO -> GPIO34\n\n");


    while (1)
    {
        int adc_value = 0;


        /*
         * Mikrofonun AO çıxışını oxuyuruq
         */
        ESP_ERROR_CHECK(
            adc_oneshot_read(
                adc_handle,
                ADC_CHANNEL_6,
                &adc_value
            )
        );


        printf("Microphone ADC: %d\n", adc_value);


        /*
         * 100 ms gözləyirik
         */
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}