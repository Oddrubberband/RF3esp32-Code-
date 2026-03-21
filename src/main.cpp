#include <cstdint>
#include <cstdio>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp32_nrf24_hal.hpp"
#include "nrf24.hpp"
#include "radio_manager.hpp"

namespace {
constexpr const char* TAG = "APP";
constexpr uint32_t kSampleRateHz = 8000;
constexpr size_t kPacketBytes = 32;
constexpr size_t kHeaderBytes = 4;
constexpr size_t kAudioBytesPerPacket = kPacketBytes - kHeaderBytes;

// Packet format:
// byte 0-1: sequence number
// byte 2:   audio data length
// byte 3:   flags (bit 0 = first packet, bit 1 = last packet)
// byte 4..: raw 8-bit audio bytes
bool sendU8Song(RadioManager& manager, const char* path)
{
    std::FILE* fp = std::fopen(path, "rb");
    if (!fp) {
        ESP_LOGE(TAG, "Could not open %s", path);
        return false;
    }

    uint8_t packet[kPacketBytes];
    uint16_t sequence = 0;

    while (true) {
        const size_t bytes_read =
            std::fread(packet + kHeaderBytes, 1, kAudioBytesPerPacket, fp);

        if (bytes_read == 0) {
            break;
        }

        packet[0] = static_cast<uint8_t>(sequence & 0xFF);
        packet[1] = static_cast<uint8_t>((sequence >> 8) & 0xFF);
        packet[2] = static_cast<uint8_t>(bytes_read);
        packet[3] = 0;

        if (sequence == 0) {
            packet[3] |= 0x01;
        }
        if (bytes_read < kAudioBytesPerPacket) {
            packet[3] |= 0x02;
        }

        if (!manager.sendPayload(packet, kHeaderBytes + bytes_read)) {
            std::fclose(fp);
            ESP_LOGE(TAG, "Transmit failed at packet %u", sequence);
            return false;
        }

        ++sequence;

        // Pace packets so the receiver can play them as 8 kHz audio.
        const uint32_t chunk_ms =
            static_cast<uint32_t>((bytes_read * 1000u) / kSampleRateHz);
        vTaskDelay(pdMS_TO_TICKS(chunk_ms > 0 ? chunk_ms : 1));

        if (bytes_read < kAudioBytesPerPacket) {
            break;
        }
    }

    std::fclose(fp);
    ESP_LOGI(TAG, "Finished sending %u packets", sequence);
    return true;
}
}

extern "C" void app_main(void);

void app_main(void)
{
    Esp32Nrf24Config config{};
    Esp32Nrf24Hal hal(config);
    Nrf24 radio(hal);
    RadioManager manager(radio);

    if (!manager.boot(76)) {
        ESP_LOGE(TAG, "Radio boot failed, state=%s fault=%d",
                 RadioManager::stateName(manager.status().state),
                 manager.status().last_fault);
        return;
    }

    ESP_LOGI(TAG, "Radio boot OK, state=%s",
             RadioManager::stateName(manager.status().state));

    sendU8Song(manager, "/spiffs/song.u8");

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
