#include <cstdint>
#include <cstdio>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "audio_packet.hpp"
#include "esp32_nrf24_hal.hpp"
#include "nrf24.hpp"
#include "radio_manager.hpp"

namespace {
constexpr const char* TAG = "APP";
constexpr uint32_t kSampleRateHz = 8000;
constexpr uint32_t kAudioBytesPerSecond = kSampleRateHz;  // 8-bit mono PCM
// Keep the stream math explicit so we can sanity-check radio throughput up front.
constexpr uint32_t kPacketsPerSecond =
    (kAudioBytesPerSecond + AudioPacket::kAudioBytesPerPacket - 1) /
    AudioPacket::kAudioBytesPerPacket;
constexpr uint32_t kPayloadBitsPerSecond =
    kPacketsPerSecond * AudioPacket::kPacketBytes * 8;

static_assert(AudioPacket::kPacketBytes <= 32, "nRF24 payload limit exceeded");
static_assert(kPayloadBitsPerSecond < 250000, "Audio stream exceeds the nRF24 250 kbps mode");

bool mountSongFs()
{
    // The demo sender streams a preconverted file from SPIFFS rather than decoding audio on-device.
    esp_vfs_spiffs_conf_t conf{};
    conf.base_path = "/spiffs";
    conf.partition_label = nullptr;
    conf.max_files = 4;
    conf.format_if_mount_failed = false;

    const esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SPIFFS: %s", esp_err_to_name(err));
        return false;
    }

    size_t total = 0;
    size_t used = 0;
    const esp_err_t info_err = esp_spiffs_info(nullptr, &total, &used);
    if (info_err == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS mounted: used=%u total=%u",
                 static_cast<unsigned>(used),
                 static_cast<unsigned>(total));
    }

    return true;
}

bool sendU8Song(RadioManager& manager, const char* path)
{
    std::FILE* fp = std::fopen(path, "rb");
    if (!fp) {
        ESP_LOGE(TAG, "Could not open %s. Put song.u8 in data/ and upload the filesystem image.", path);
        return false;
    }

    uint8_t audio_chunk[AudioPacket::kAudioBytesPerPacket];
    uint8_t packet[AudioPacket::kPacketBytes];
    uint16_t sequence = 0;

    while (true) {
        // Each radio packet carries one small chunk of raw 8-bit PCM plus a header.
        const size_t bytes_read =
            std::fread(audio_chunk, 1, AudioPacket::kAudioBytesPerPacket, fp);

        if (bytes_read == 0) {
            break;
        }

        size_t packet_len = 0;
        const bool is_last = bytes_read < AudioPacket::kAudioBytesPerPacket;
        if (!AudioPacket::encode(sequence,
                                 audio_chunk,
                                 bytes_read,
                                 sequence == 0,
                                 is_last,
                                 packet,
                                 packet_len)) {
            std::fclose(fp);
            ESP_LOGE(TAG, "Failed to build packet %u", sequence);
            return false;
        }

        if (!manager.sendPayload(packet, packet_len)) {
            std::fclose(fp);
            ESP_LOGE(TAG, "Transmit failed at packet %u", sequence);
            return false;
        }

        ++sequence;

        // Rough pacing keeps send rate close to the time represented by this chunk.
        const uint32_t chunk_ms =
            static_cast<uint32_t>((bytes_read * 1000u) / kSampleRateHz);
        vTaskDelay(pdMS_TO_TICKS(chunk_ms > 0 ? chunk_ms : 1));

        if (is_last) {
            break;
        }
    }

    std::fclose(fp);
    ESP_LOGI(TAG, "Finished sending %u packets", sequence);
    return true;
}
}  // namespace

extern "C" void app_main(void);

void app_main(void)
{
    // Boot order: mount storage, bring up the radio, then send the demo file once.
    Esp32Nrf24Config config{};
    Esp32Nrf24Hal hal(config);
    Nrf24 radio(hal);
    RadioManager manager(radio);

    if (!mountSongFs()) {
        ESP_LOGE(TAG, "Mount SPIFFS failed. Upload a filesystem image first.");
        return;
    }

    if (!manager.boot(76)) {
        ESP_LOGE(TAG, "Radio boot failed, state=%s fault=%d",
                 RadioManager::stateName(manager.status().state),
                 manager.status().last_fault);
        return;
    }

    ESP_LOGI(TAG, "Radio boot OK, state=%s",
             RadioManager::stateName(manager.status().state));
    ESP_LOGI(TAG, "Audio stream: %u bytes/sec, ~%u packets/sec, ~%u payload bits/sec",
             static_cast<unsigned>(kAudioBytesPerSecond),
             static_cast<unsigned>(kPacketsPerSecond),
             static_cast<unsigned>(kPayloadBitsPerSecond));

    sendU8Song(manager, "/spiffs/song.u8");

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
