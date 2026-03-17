#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp32_nrf24_hal.hpp"
#include "nrf24.hpp"

extern "C" void app_main(void);

void app_main(void)
{
    static const char* TAG = "APP";

    Esp32Nrf24Hal hal(
        SPI3_HOST,
        GPIO_NUM_18,   // SCK
        GPIO_NUM_23,   // MOSI
        GPIO_NUM_19,   // MISO
        GPIO_NUM_27,   // CE
        GPIO_NUM_17    // CSN
    );

    Nrf24 radio(hal);

    bool ok = radio.begin();
    ESP_LOGI(TAG, "begin() = %s", ok ? "true" : "false");
    ESP_LOGI(TAG, "STATUS = 0x%02X", radio.status());
    ESP_LOGI(TAG, "CONFIG = 0x%02X", radio.readReg(0x00));
    ESP_LOGI(TAG, "RF_CH  = 0x%02X", radio.readReg(0x05));
    ESP_LOGI(TAG, "RF_SETUP = 0x%02X", radio.readReg(0x06));

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}