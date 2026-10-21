#include <stdio.h>
#include "driver/i2c.h"

#define I2C_MASTER_SCL_IO           22    // SCL pin
#define I2C_MASTER_SDA_IO           21    // SDA pin
#define I2C_MASTER_NUM              I2C_NUM_0
#define I2C_MASTER_FREQ_HZ          100000
#define I2C_MASTER_TX_BUF_DISABLE   0
#define I2C_MASTER_RX_BUF_DISABLE   0

// Init I2C master
static void i2c_master_init() {
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };
    i2c_param_config(I2C_MASTER_NUM, &conf);
    i2c_driver_install(I2C_MASTER_NUM, conf.mode,
                       I2C_MASTER_RX_BUF_DISABLE,
                       I2C_MASTER_TX_BUF_DISABLE, 0);
}

// Write one byte to device register
esp_err_t i2c_write_byte(uint8_t dev_addr, uint8_t reg_addr, uint8_t data) {
    return i2c_master_write_to_device(I2C_MASTER_NUM, dev_addr,
                                      (uint8_t[]){reg_addr, data}, 2,
                                      1000 / portTICK_PERIOD_MS);
}

// Read one byte from device register
esp_err_t i2c_read_byte(uint8_t dev_addr, uint8_t reg_addr, uint8_t *data) {
    return i2c_master_write_read_device(I2C_MASTER_NUM, dev_addr,
                                        &reg_addr, 1, data, 1,
                                        1000 / portTICK_PERIOD_MS);
}

// Read multiple bytes
esp_err_t i2c_read_bytes(uint8_t dev_addr, uint8_t reg_addr,
                         uint8_t *data, size_t len) {
    return i2c_master_write_read_device(I2C_MASTER_NUM, dev_addr,
                                        &reg_addr, 1, data, len,
                                        1000 / portTICK_PERIOD_MS);
}

void app_main() {
    i2c_master_init();
    printf("Universal I2C Template Ready!\n");

    uint8_t whoami;
    if (i2c_read_byte(0x68, 0x75, &whoami) == ESP_OK) {
        printf("MPU6050 WHO_AM_I: 0x%02X\n", whoami);
    } else {
        printf("Read error!\n");
    }
}
