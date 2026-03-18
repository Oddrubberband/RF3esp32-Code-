#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp32_nrf24_hal.hpp"
#include "nrf24.hpp"
#include "radio_manager.hpp"

extern "C" void app_main(void);

void app_main(void)
{
    static const char* TAG = "APP";

    // These are your already-chosen wiring values.
    Esp32Nrf24Config config{};

    Esp32Nrf24Hal hal(config);

    // TODO:
    Nrf24 radio(hal);
    RadioManager manager(radio);

    ESP_LOGI(TAG, "Firmware scaffold started");

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}