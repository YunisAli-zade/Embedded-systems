#include "driver/spi_master.h"
#include "esp_log.h"

#define PIN_NUM_MISO 19
#define PIN_NUM_MOSI 23
#define PIN_NUM_CLK  18
#define PIN_NUM_CS   5

static const char *TAG = "SPI_METHODS";

spi_device_handle_t spi;  // Global handle

// 1. Bus initialize methode
void init_spi_bus(void) {
    spi_bus_config_t buscfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096
    };
    ESP_ERROR_CHECK(spi_bus_initialize(HSPI_HOST, &buscfg, SPI_DMA_CH_AUTO));
}

// 2. Device initialize methode
void init_spi_device(void) {
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 10 * 1000 * 1000,   // 10 MHz
        .mode = 0,                            // SPI mode 0
        .spics_io_num = PIN_NUM_CS,
        .queue_size = 7,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(HSPI_HOST, &devcfg, &spi));
}

// 3. Transaction send methode
uint8_t spi_send_recv(uint8_t data) {
    uint8_t recvData = 0;
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = 8;              // 8 bit
    t.tx_buffer = &data;
    t.rx_buffer = &recvData;

    ESP_ERROR_CHECK(spi_device_transmit(spi, &t));
    return recvData;
}

// 4. Main
void app_main(void) {
    init_spi_bus();
    init_spi_device();

    uint8_t sendData = 0x42;
    uint8_t recvData = spi_send_recv(sendData);

    ESP_LOGI(TAG, "Sent: %02X, Received: %02X", sendData, recvData);
}
