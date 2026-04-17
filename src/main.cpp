#include <algorithm>
#include <array>
#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <vector>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_spiffs.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "audio_packet.hpp"
#include "esp32_nrf24_hal.hpp"
#include "morse.hpp"
#include "nrf24.hpp"
#include "radio_manager.hpp"
#include "validation.hpp"
#include "stream_sync.hpp"
#include "wifi_control_config.hpp"

#ifndef WIRELESS_CONTROL_ENABLED
#define WIRELESS_CONTROL_ENABLED 0
#endif

#ifndef WIRELESS_CONTROL_AUTO_RX
#define WIRELESS_CONTROL_AUTO_RX 0
#endif

// main.cpp owns the top-level demo flow:
// - mount the Serial Peripheral Interface Flash File System (SPIFFS) partition
//   that stores converted audio
// - initialize the nRF24 radio
// - expose a serial-console command loop
// - run a small background task that polls receive (RX) mode
//
// Most of the project's user-visible behavior lives here, while lower-level
// files keep packet formatting and radio access focused and testable.
namespace {
constexpr const char* TAG = "APP";
// The demo audio format is unsigned 8-bit mono pulse-code modulation (PCM) at
// 8 kHz, so one byte is consumed per sample.
constexpr uint32_t kSampleRateHz = 8000;
constexpr uint32_t kAudioBytesPerSecond = kSampleRateHz;
constexpr uint32_t kSampleBytePeriodUs = 1000000u / kSampleRateHz;
// Packet rate is derived from the packet payload budget.
constexpr uint32_t kPacketsPerSecond =
    (kAudioBytesPerSecond + AudioPacket::kAudioBytesPerPacket - 1) /
    AudioPacket::kAudioBytesPerPacket;
// Rough over-the-air bitrate including the packet header bytes.
constexpr uint32_t kPayloadBitsPerSecond =
    kPacketsPerSecond * AudioPacket::kPacketBytes * 8;
constexpr const char* kSpiffsRoot = "/spiffs";
constexpr const char* kDefaultTrack = "song.u8";
constexpr uint32_t kDefaultMorseDotMs = 120;
constexpr uint8_t kDefaultMorsePowerLevel = 3;
constexpr TickType_t kLoopWorkerPeriod = pdMS_TO_TICKS(20);
constexpr TickType_t kLoopStopPollPeriod = pdMS_TO_TICKS(20);
constexpr TickType_t kLoopStopTimeout = pdMS_TO_TICKS(2000);
constexpr TickType_t kWifiControlPollPeriod = pdMS_TO_TICKS(100);
// The nRF24 RX FIFO is only three packets deep, so polling much slower than
// the packet cadence will overrun the queue during live audio traffic.
constexpr TickType_t kRxPollPeriod = pdMS_TO_TICKS(2) > 0 ? pdMS_TO_TICKS(2) : 1;
constexpr TickType_t kWifiConnectTimeout = pdMS_TO_TICKS(15000);
constexpr size_t kConsoleLineBytes = 160;
constexpr bool kWirelessControlEnabled = WIRELESS_CONTROL_ENABLED != 0;
constexpr bool kWirelessControlAutoRx = WIRELESS_CONTROL_AUTO_RX != 0;
constexpr EventBits_t kWifiConnectedBit = BIT0;
constexpr char kWifiPlaceholderSsid[] = "YOUR_WIFI_SSID";
constexpr char kWifiPlaceholderPassword[] = "YOUR_WIFI_PASSWORD";

// These compile-time checks keep the audio format aligned with nRF24 hardware
// limits instead of failing later at runtime.
static_assert(AudioPacket::kPacketBytes <= 32, "nRF24 payload limit exceeded");
static_assert(kPayloadBitsPerSecond < 250000, "Audio stream exceeds the nRF24 250 kbps mode");
static_assert((1000000u % kSampleRateHz) == 0, "Sample timing must be an integer number of microseconds");

struct TrackInfo {
    std::string name;
    size_t bytes = 0;
};

enum class LoopMode {
    None,
    Tx,
    Cw,
    Morse
};

enum class CommandOrigin {
    Local,
    Remote,
    Http
};

struct LoopConfig {
    LoopMode mode = LoopMode::None;
    bool active = false;
    bool infinite = false;
    bool restore_rx_after_completion = false;
    uint32_t remaining_iterations = 0;
    uint32_t completed_iterations = 0;
    std::string track_name;
    std::string morse_text;
    uint32_t cw_on_ms = 0;
    uint32_t cw_off_ms = 0;
    uint32_t cw_report_every = 0;
    uint8_t channel = 76;
    uint8_t power_level = 3;
};

struct HttpStatusSnapshot {
    const char* node_name = WifiControlConfig::kNodeName;
    const char* hostname = WifiControlConfig::kNodeName;
    const char* state_name = "Boot";
    uint8_t channel = 76;
    bool tx_ok = false;
    bool tx_timeout = false;
    uint32_t rx_packets = 0;
    uint32_t rx_audio = 0;
    uint32_t rx_raw = 0;
    int last_fault = 0;
};

const char* loopModeName(LoopMode mode)
{
    switch (mode) {
        case LoopMode::Tx: return "TxLoop";
        case LoopMode::Cw: return "CwLoop";
        case LoopMode::Morse: return "Morse";
        case LoopMode::None:
        default:
            return "None";
    }
}

// ---- small string and parsing helpers ------------------------------------

std::string trimAscii(std::string value)
{
    // Command parsing is intentionally American Standard Code for Information
    // Interchange (ASCII)-centric because all console commands and filenames
    // in this project are plain ASCII tokens.
    const auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
    while (!value.empty() && is_space(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() && is_space(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    return value;
}

std::string uppercaseCopy(std::string_view text)
{
    // Commands are matched case-insensitively by normalizing once up front.
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return out;
}

bool endsWithIgnoreCase(std::string_view text, std::string_view suffix)
{
    // Track selection accepts file names in any case but still enforces the
    // expected .u8 extension.
    if (text.size() < suffix.size()) {
        return false;
    }

    const size_t offset = text.size() - suffix.size();
    for (size_t i = 0; i < suffix.size(); ++i) {
        const unsigned char lhs = static_cast<unsigned char>(text[offset + i]);
        const unsigned char rhs = static_cast<unsigned char>(suffix[i]);
        if (std::tolower(lhs) != std::tolower(rhs)) {
            return false;
        }
    }

    return true;
}

std::vector<std::string> splitWords(const std::string& line)
{
    // The console syntax is intentionally simple: tokens are whitespace
    // separated, with no quoting or escaping rules.
    std::vector<std::string> words;
    std::string current;

    for (char ch : line) {
        if (std::isspace(static_cast<unsigned char>(ch))) {
            if (!current.empty()) {
                words.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(ch);
    }

    if (!current.empty()) {
        words.push_back(current);
    }

    return words;
}

bool parseUint8Arg(std::string_view text, uint8_t minimum, uint8_t maximum, uint8_t& out)
{
    // Many console commands take one byte-sized numeric argument such as a
    // channel or power level, so this helper centralizes the range checking.
    if (text.empty()) {
        return false;
    }

    char* end = nullptr;
    const std::string value(text);
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (!end || *end != '\0' || parsed < minimum || parsed > maximum) {
        return false;
    }

    out = static_cast<uint8_t>(parsed);
    return true;
}

bool parseUint32Arg(std::string_view text, uint32_t minimum, uint32_t maximum, uint32_t& out)
{
    if (text.empty()) {
        return false;
    }

    char* end = nullptr;
    const std::string value(text);
    const unsigned long parsed = std::strtoul(value.c_str(), &end, 10);
    if (!end || *end != '\0' || parsed < minimum || parsed > maximum) {
        return false;
    }

    out = static_cast<uint32_t>(parsed);
    return true;
}

bool parseLoopCountToken(std::string_view text, bool& infinite, uint32_t& count)
{
    const std::string upper = uppercaseCopy(text);
    if (upper == "INF" || upper == "FOREVER") {
        infinite = true;
        count = 0;
        return true;
    }

    uint32_t parsed = 0;
    if (!parseUint32Arg(text, 1, UINT32_MAX, parsed)) {
        return false;
    }

    infinite = false;
    count = parsed;
    return true;
}

bool statFileSize(const std::string& path, size_t& bytes)
{
    // SPIFFS directory listings only provide file names. stat() is used to get
    // an accurate byte count for user-interface (UI) output and track
    // selection.
    struct stat info {};
    if (::stat(path.c_str(), &info) != 0) {
        return false;
    }
    if ((info.st_mode & S_IFREG) == 0) {
        return false;
    }
    bytes = static_cast<size_t>(info.st_size);
    return true;
}

double durationSeconds(size_t bytes)
{
    // Duration is derived directly from the known pulse-code modulation (PCM)
    // format: one byte per sample at 8 kHz.
    return static_cast<double>(bytes) / static_cast<double>(kAudioBytesPerSecond);
}

void delayAtLeastMs(uint32_t duration_ms)
{
    const TickType_t ticks = pdMS_TO_TICKS(duration_ms);
    vTaskDelay(ticks > 0 ? ticks : 1);
}

void appendFormat(std::string& out, const char* format, ...)
{
    va_list args;
    va_start(args, format);

    va_list copy;
    va_copy(copy, args);
    const int needed = std::vsnprintf(nullptr, 0, format, copy);
    va_end(copy);

    if (needed > 0) {
        const size_t start = out.size();
        out.resize(start + static_cast<size_t>(needed));
        std::vsnprintf(out.data() + start,
                       static_cast<size_t>(needed) + 1,
                       format,
                       args);
    }

    va_end(args);
}

// ---- SPIFFS track discovery helpers --------------------------------------

std::string buildTrackPath(std::string_view name)
{
    // Accept either a bare file name or an already-qualified /spiffs path so
    // higher-level code can stay flexible.
    if (name.rfind("/spiffs/", 0) == 0) {
        return std::string(name);
    }
    return std::string(kSpiffsRoot) + "/" + std::string(name);
}

bool resolveTrack(std::string request, TrackInfo& out)
{
    // Resolve a user-facing track reference into a verified SPIFFS file.
    //
    // The console accepts either "song" or "song.u8". Directory separators are
    // rejected so commands cannot escape the SPIFFS root.
    request = trimAscii(request);
    if (request.empty()) {
        return false;
    }
    if (request.rfind("/spiffs/", 0) == 0) {
        request.erase(0, std::strlen("/spiffs/"));
    }
    if (request.find('/') != std::string::npos || request.find('\\') != std::string::npos) {
        return false;
    }

    std::vector<std::string> candidates{request};
    if (!endsWithIgnoreCase(request, ".u8")) {
        candidates.push_back(request + ".u8");
    }

    for (const std::string& candidate : candidates) {
        size_t bytes = 0;
        if (statFileSize(buildTrackPath(candidate), bytes)) {
            out.name = candidate;
            out.bytes = bytes;
            return true;
        }
    }

    return false;
}

std::vector<TrackInfo> listTracks()
{
    // Build a stable, sorted view of the available .u8 files so the console can
    // list them predictably.
    std::vector<TrackInfo> tracks;
    DIR* dir = opendir(kSpiffsRoot);
    if (!dir) {
        return tracks;
    }

    while (dirent* entry = readdir(dir)) {
        if (entry->d_name[0] == '.') {
            continue;
        }

        const std::string name(entry->d_name);
        if (!endsWithIgnoreCase(name, ".u8")) {
            continue;
        }

        size_t bytes = 0;
        if (!statFileSize(buildTrackPath(name), bytes)) {
            continue;
        }

        tracks.push_back({name, bytes});
    }

    closedir(dir);
    std::sort(tracks.begin(), tracks.end(), [](const TrackInfo& lhs, const TrackInfo& rhs) {
        return lhs.name < rhs.name;
    });
    return tracks;
}

void printTrackTable(const std::vector<TrackInfo>& tracks, std::string_view selected_track)
{
    // Show every available file and mark the one transmit (TX) will use by
    // default.
    if (tracks.empty()) {
        std::printf("No .u8 tracks are available in SPIFFS.\n");
        return;
    }

    std::printf("Tracks in SPIFFS:\n");
    for (const TrackInfo& track : tracks) {
        const char* marker = track.name == selected_track ? "*" : " ";
        std::printf(" %s %s (%u bytes, %.1f s)\n",
                    marker,
                    track.name.c_str(),
                    static_cast<unsigned>(track.bytes),
                    durationSeconds(track.bytes));
    }
}

// ---- startup helpers ------------------------------------------------------

bool mountSongFs()
{
    // The app expects a prebuilt SPIFFS image containing converted audio files.
    // It does not auto-format on failure because an empty partition is more
    // likely a missing upload than genuine corruption.
    esp_vfs_spiffs_conf_t conf{};
    conf.base_path = kSpiffsRoot;
    conf.partition_label = nullptr;
    conf.max_files = 8;
    conf.format_if_mount_failed = false;

    const esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SPIFFS: %s", esp_err_to_name(err));
        return false;
    }

    size_t total = 0;
    size_t used = 0;
    if (esp_spiffs_info(nullptr, &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS mounted: used=%u total=%u",
                 static_cast<unsigned>(used),
                 static_cast<unsigned>(total));
    }

    return true;
}

bool sendU8Song(RadioManager& manager,
                const char* path,
                const volatile bool* stop_requested = nullptr)
{
    std::FILE* fp = std::fopen(path, "rb");
    if (!fp) {
        ESP_LOGE(TAG, "Could not open %s. Use the Prepare Demo Audio task and upload the filesystem image.", path);
        return false;
    }

    static uint16_t next_stream_id = 0;
    next_stream_id = static_cast<uint16_t>(next_stream_id + 1u);
    if (next_stream_id == 0) {
        next_stream_id = 1;
    }
    const uint16_t stream_id = next_stream_id;

    uint8_t control_packet[AudioPacket::kPacketBytes] = {};
    size_t control_len = 0;
    if (!StreamSync::encodeStart(stream_id, control_packet, control_len)) {
        std::fclose(fp);
        ESP_LOGE(TAG, "Failed to build START packet for stream %u", stream_id);
        return false;
    }

    for (uint8_t i = 0; i < StreamSync::kRecommendedStartRepeats; ++i) {
        if (stop_requested && *stop_requested) {
            std::fclose(fp);
            ESP_LOGI(TAG, "Stopped TX before audio start for stream %u", stream_id);
            return false;
        }

        if (!manager.sendPayload(control_packet, control_len)) {
            std::fclose(fp);
            const RadioStatus status = manager.status();
            ESP_LOGE(TAG,
                     "START transmit failed for stream %u, STATUS=0x%02X FIFO=0x%02X OBSERVE_TX=0x%02X",
                     stream_id,
                     static_cast<unsigned>(status.last_status),
                     static_cast<unsigned>(status.last_fifo_status),
                     static_cast<unsigned>(status.last_observe_tx));
            return false;
        }

        if ((i + 1) < StreamSync::kRecommendedStartRepeats) {
            delayAtLeastMs(StreamSync::kRecommendedStartGapMs);
        }
    }

    delayAtLeastMs(StreamSync::kRecommendedPostStartGapMs);

    uint8_t audio_chunk[AudioPacket::kAudioBytesPerPacket];
    uint8_t packet[AudioPacket::kPacketBytes];
    uint16_t sequence = 0;
    uint32_t pacing_remainder_us = 0;

    while (true) {
        if (stop_requested && *stop_requested) {
            std::fclose(fp);
            ESP_LOGI(TAG, "Stopped TX at packet %u for stream %u", sequence, stream_id);
            return false;
        }

        const size_t bytes_read =
            std::fread(audio_chunk, 1, AudioPacket::kAudioBytesPerPacket, fp);
        if (bytes_read == 0) {
            break;
        }

        size_t packet_len = 0;
        const bool is_last = bytes_read < AudioPacket::kAudioBytesPerPacket;

        std::fill(packet, packet + AudioPacket::kPacketBytes, 0);

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

        if (!manager.sendPayload(packet, AudioPacket::kPacketBytes)) {
            std::fclose(fp);
            const RadioStatus status = manager.status();
            ESP_LOGE(TAG,
                     "Transmit failed at packet %u, STATUS=0x%02X FIFO=0x%02X OBSERVE_TX=0x%02X IRQ=%s tx_irq_seen=%s timeout=%s",
                     sequence,
                     static_cast<unsigned>(status.last_status),
                     static_cast<unsigned>(status.last_fifo_status),
                     static_cast<unsigned>(status.last_observe_tx),
                     !status.irq_connected ? "disabled" : (status.irq_asserted ? "low" : "high"),
                     status.last_tx_saw_irq ? "true" : "false",
                     status.last_tx_timed_out ? "true" : "false");
            return false;
        }

        if (sequence == 0) {
            if (stop_requested && *stop_requested) {
                std::fclose(fp);
                ESP_LOGI(TAG, "Stopped TX during sequence 0 duplication for stream %u", stream_id);
                return false;
            }

            uint8_t duplicate_packet[AudioPacket::kPacketBytes];
            std::copy(packet, packet + AudioPacket::kPacketBytes, duplicate_packet);
            duplicate_packet[AudioPacket::kPacketBytes - 1] ^= 0xA5;

            delayAtLeastMs(StreamSync::kRecommendedSeq0DuplicateGapMs);

            if (!manager.sendPayload(duplicate_packet, AudioPacket::kPacketBytes)) {
                std::fclose(fp);
                const RadioStatus status = manager.status();
                ESP_LOGE(TAG,
                         "Sequence 0 duplicate failed, STATUS=0x%02X FIFO=0x%02X OBSERVE_TX=0x%02X",
                         static_cast<unsigned>(status.last_status),
                         static_cast<unsigned>(status.last_fifo_status),
                         static_cast<unsigned>(status.last_observe_tx));
                return false;
            }
        }

        ++sequence;

        const uint32_t chunk_total_us =
            static_cast<uint32_t>(bytes_read) * 125u + pacing_remainder_us;
        const uint32_t chunk_ms = chunk_total_us / 1000u;
        pacing_remainder_us = chunk_total_us % 1000u;

        if (chunk_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(chunk_ms));
        }

        if (is_last) {
            break;
        }
    }

    if (StreamSync::encodeStop(stream_id, control_packet, control_len)) {
        (void)manager.sendPayload(control_packet, control_len);
    }

    std::fclose(fp);
    ESP_LOGI(TAG, "Finished sending %u packets for stream %u", sequence, stream_id);
    return true;
}

class DemoConsoleApp {
public:
    explicit DemoConsoleApp(RadioManager& manager)
        : manager_(manager)
    {
    }

    bool initialize()
    {
        // Initialization wires together every subsystem needed before the
        // interactive console can start:
        // - create a mutex that serializes all radio access
        // - disable stdio buffering so the serial console feels live
        // - mount SPIFFS and verify the selected track exists
        // - boot the radio
        // - start the background receive (RX) polling task
        radio_mutex_ = xSemaphoreCreateMutex();
        if (!radio_mutex_) {
            ESP_LOGE(TAG, "Failed to allocate radio mutex");
            return false;
        }

        loop_mutex_ = xSemaphoreCreateMutex();
        if (!loop_mutex_) {
            ESP_LOGE(TAG, "Failed to allocate loop mutex");
            return false;
        }

        command_mutex_ = xSemaphoreCreateMutex();
        if (!command_mutex_) {
            ESP_LOGE(TAG, "Failed to allocate command mutex");
            return false;
        }

        std::setvbuf(stdin, nullptr, _IONBF, 0);
        std::setvbuf(stdout, nullptr, _IONBF, 0);

        if (!mountSongFs()) {
            ESP_LOGE(TAG, "Mount SPIFFS failed. Upload a filesystem image first.");
            return false;
        }

        if (!takeRadio()) {
            ESP_LOGE(TAG, "Could not acquire radio mutex during init");
            return false;
        }

        // Channel 76 is used as the demo default, but the console can
        // reinitialize on another channel later.
        const bool boot_ok = manager_.boot(76);
        const RadioStatus status = manager_.status();
        giveRadio();

        if (!boot_ok) {
            ESP_LOGW(TAG, "Radio boot failed, state=%s fault=%d",
                     RadioManager::stateName(status.state),
                     status.last_fault);
            ESP_LOGW(TAG, "Probe snapshot: STATUS=0x%02X", static_cast<unsigned>(status.last_status));
            if (status.last_status == 0x00) {
                ESP_LOGW(TAG, "STATUS=0x00 usually means the nRF24 is unpowered, MISO is held low, or CSN/SCK/MOSI/MISO wiring is not reaching the chip.");
            } else if (status.last_status == 0xFF) {
                ESP_LOGW(TAG, "STATUS=0xFF usually means CSN is not selecting the radio or MISO is floating high.");
            }
            ESP_LOGW(TAG, "Continuing without radio so SPIFFS and the serial console remain testable.");
        }

        // If the configured default track is missing, fall back to the first
        // available SPIFFS file so the transmit (TX) command still has a sane
        // default.
        const std::vector<TrackInfo> tracks = listTracks();
        if (!tracks.empty()) {
            TrackInfo selected{};
            if (!resolveTrack(selected_track_, selected)) {
                selected_track_ = tracks.front().name;
            }
        }

        if (xTaskCreate(&DemoConsoleApp::rxTaskEntry, "radio_rx", 4096, this, 4, &rx_task_) != pdPASS) {
            ESP_LOGE(TAG, "Failed to start RX polling task");
            return false;
        }

        if (xTaskCreate(&DemoConsoleApp::loopTaskEntry, "radio_loop", 4096, this, 4, &loop_task_) != pdPASS) {
            ESP_LOGE(TAG, "Failed to start loop task");
            return false;
        }

        if (boot_ok) {
            ESP_LOGI(TAG, "Radio boot OK, state=%s", RadioManager::stateName(status.state));
        } else {
            ESP_LOGI(TAG, "Radio unavailable, console running in filesystem-only mode.");
        }

        if (boot_ok && kWirelessControlEnabled && kWirelessControlAutoRx) {
            tryResumeWirelessRx("Wireless control armed");
        }

        if (!startWifiControlPlane()) {
            ESP_LOGW(TAG, "Wi-Fi control plane not available. Serial console remains active.");
        }

        ESP_LOGI(TAG, "Audio stream: %u bytes/sec, ~%u packets/sec, ~%u payload bits/sec",
                 static_cast<unsigned>(kAudioBytesPerSecond),
                 static_cast<unsigned>(kPacketsPerSecond),
                 static_cast<unsigned>(kPayloadBitsPerSecond));
        printHelp();
        printTrackTable(tracks, selected_track_);
        printStatus();
        return true;
    }

    void run()
    {
        // UART-backed stdin may deliver one character at a time depending on
        // the host monitor. Accumulate characters locally so commands are only
        // dispatched after Enter is pressed.
        std::string pending_line;
        pending_line.reserve(kConsoleLineBytes);
        bool prompt_visible = false;

        while (true) {
            if (!prompt_visible) {
                printPrompt();
                prompt_visible = true;
            }

            const int raw = std::fgetc(stdin);
            if (raw == EOF) {
                clearerr(stdin);
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }

            const char ch = static_cast<char>(raw);
            if (ch == '\r' || ch == '\n') {
                std::printf("\n");
                std::fflush(stdout);
                const std::string command_line = trimAscii(pending_line);
                pending_line.clear();
                if (!command_line.empty()) {
                    dispatchCommand(command_line, CommandOrigin::Local);
                }
                prompt_visible = false;
                continue;
            }

            if (ch == '\b' || static_cast<unsigned char>(ch) == 0x7F) {
                if (!pending_line.empty()) {
                    pending_line.pop_back();
                    std::printf("\b \b");
                    std::fflush(stdout);
                }
                continue;
            }

            if (pending_line.size() + 1 < kConsoleLineBytes) {
                pending_line.push_back(ch);
                std::printf("%c", ch);
                std::fflush(stdout);
            }
        }
    }

private:
    static void rxTaskEntry(void* ctx)
    {
        // Free Real-Time Operating System (FreeRTOS) tasks must start from a
        // plain C-style entry point, so this static trampoline forwards to the
        // class instance.
        static_cast<DemoConsoleApp*>(ctx)->rxTask();
    }

    static void loopTaskEntry(void* ctx)
    {
        static_cast<DemoConsoleApp*>(ctx)->loopTask();
    }

    static void wifiControlTaskEntry(void* ctx)
    {
        static_cast<DemoConsoleApp*>(ctx)->wifiControlTask();
    }

    static void wifiEventHandlerEntry(void* arg,
                                      esp_event_base_t event_base,
                                      int32_t event_id,
                                      void* event_data)
    {
        static_cast<DemoConsoleApp*>(arg)->handleWifiEvent(event_base, event_id, event_data);
    }

    static esp_err_t httpStatusHandlerEntry(httpd_req_t* req)
    {
        return static_cast<DemoConsoleApp*>(req->user_ctx)->handleHttpStatus(req);
    }

    static esp_err_t httpRxStartHandlerEntry(httpd_req_t* req)
    {
        return static_cast<DemoConsoleApp*>(req->user_ctx)->handleHttpRxStart(req);
    }

    static esp_err_t httpTxStartHandlerEntry(httpd_req_t* req)
    {
        return static_cast<DemoConsoleApp*>(req->user_ctx)->handleHttpTxStart(req);
    }

    static esp_err_t httpStopHandlerEntry(httpd_req_t* req)
    {
        return static_cast<DemoConsoleApp*>(req->user_ctx)->handleHttpStop(req);
    }

    static esp_err_t httpChannelHandlerEntry(httpd_req_t* req)
    {
        return static_cast<DemoConsoleApp*>(req->user_ctx)->handleHttpChannel(req);
    }

    esp_err_t sendHttpJson(httpd_req_t* req,
                           const std::string& json,
                           const char* status = "200 OK") const
    {
        httpd_resp_set_status(req, status);
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, json.c_str(), HTTPD_RESP_USE_STRLEN);
    }

    esp_err_t sendHttpStatusResponse(httpd_req_t* req, const char* status = "200 OK")
    {
        const std::string json = buildStatusJson();
        return sendHttpJson(req, json, status);
    }

    bool startHttpServer()
    {
        if (http_server_) {
            return true;
        }

        httpd_config_t config = HTTPD_DEFAULT_CONFIG();
        config.server_port = WifiControlConfig::kHttpPort;

        esp_err_t err = httpd_start(&http_server_, &config);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "HTTP server start failed: %s", esp_err_to_name(err));
            http_server_ = nullptr;
            return false;
        }

        httpd_uri_t status_uri{};
        status_uri.uri = "/status";
        status_uri.method = HTTP_GET;
        status_uri.handler = &DemoConsoleApp::httpStatusHandlerEntry;
        status_uri.user_ctx = this;

        httpd_uri_t rx_uri{};
        rx_uri.uri = "/rx/start";
        rx_uri.method = HTTP_POST;
        rx_uri.handler = &DemoConsoleApp::httpRxStartHandlerEntry;
        rx_uri.user_ctx = this;

        httpd_uri_t tx_uri{};
        tx_uri.uri = "/tx/start";
        tx_uri.method = HTTP_POST;
        tx_uri.handler = &DemoConsoleApp::httpTxStartHandlerEntry;
        tx_uri.user_ctx = this;

        httpd_uri_t stop_uri{};
        stop_uri.uri = "/stop";
        stop_uri.method = HTTP_POST;
        stop_uri.handler = &DemoConsoleApp::httpStopHandlerEntry;
        stop_uri.user_ctx = this;

        httpd_uri_t channel_uri{};
        channel_uri.uri = "/channel";
        channel_uri.method = HTTP_POST;
        channel_uri.handler = &DemoConsoleApp::httpChannelHandlerEntry;
        channel_uri.user_ctx = this;

        err = httpd_register_uri_handler(http_server_, &status_uri);
        if (err == ESP_OK) err = httpd_register_uri_handler(http_server_, &rx_uri);
        if (err == ESP_OK) err = httpd_register_uri_handler(http_server_, &tx_uri);
        if (err == ESP_OK) err = httpd_register_uri_handler(http_server_, &stop_uri);
        if (err == ESP_OK) err = httpd_register_uri_handler(http_server_, &channel_uri);

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "HTTP route registration failed: %s", esp_err_to_name(err));
            httpd_stop(http_server_);
            http_server_ = nullptr;
            return false;
        }

        ESP_LOGI(TAG, "HTTP control server listening on port %u",
                 static_cast<unsigned>(WifiControlConfig::kHttpPort));
        return true;
    }

    bool startWifiControlPlane()
    {
        if (std::strcmp(WifiControlConfig::kSsid, kWifiPlaceholderSsid) == 0 ||
            std::strcmp(WifiControlConfig::kPassword, kWifiPlaceholderPassword) == 0 ||
            WifiControlConfig::kSsid[0] == '\0') {
            ESP_LOGW(TAG,
                     "Wi-Fi credentials not configured. Update include/wifi_control_config.hpp or define RF3_WIFI_SSID/RF3_WIFI_PASSWORD at build time.");
            return false;
        }

        esp_err_t err = nvs_flash_init();
        if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            ESP_ERROR_CHECK(nvs_flash_erase());
            err = nvs_flash_init();
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "NVS init failed for Wi-Fi: %s", esp_err_to_name(err));
            return false;
        }

        err = esp_netif_init();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
            return false;
        }

        err = esp_event_loop_create_default();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(err));
            return false;
        }

        wifi_event_group_ = xEventGroupCreate();
        if (!wifi_event_group_) {
            ESP_LOGE(TAG, "Could not allocate Wi-Fi event group");
            return false;
        }

        if (!wifi_control_task_) {
            if (xTaskCreate(&DemoConsoleApp::wifiControlTaskEntry,
                            "wifi_ctrl",
                            4096,
                            this,
                            4,
                            &wifi_control_task_) != pdPASS) {
                ESP_LOGE(TAG, "Failed to start Wi-Fi control task");
                return false;
            }
        }

        wifi_netif_ = esp_netif_create_default_wifi_sta();
        if (!wifi_netif_) {
            ESP_LOGE(TAG, "Could not create default Wi-Fi station interface");
            return false;
        }

        err = esp_netif_set_hostname(wifi_netif_, WifiControlConfig::kNodeName);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Could not set hostname '%s': %s",
                     WifiControlConfig::kNodeName,
                     esp_err_to_name(err));
        }

        wifi_init_config_t wifi_init = WIFI_INIT_CONFIG_DEFAULT();
        err = esp_wifi_init(&wifi_init);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
            return false;
        }

        err = esp_event_handler_instance_register(WIFI_EVENT,
                                                  ESP_EVENT_ANY_ID,
                                                  &DemoConsoleApp::wifiEventHandlerEntry,
                                                  this,
                                                  &wifi_event_handler_);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Wi-Fi event handler register failed: %s", esp_err_to_name(err));
            return false;
        }

        err = esp_event_handler_instance_register(IP_EVENT,
                                                  IP_EVENT_STA_GOT_IP,
                                                  &DemoConsoleApp::wifiEventHandlerEntry,
                                                  this,
                                                  &ip_event_handler_);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "IP event handler register failed: %s", esp_err_to_name(err));
            return false;
        }

        wifi_config_t wifi_config{};
        std::snprintf(reinterpret_cast<char*>(wifi_config.sta.ssid),
                      sizeof(wifi_config.sta.ssid),
                      "%s",
                      WifiControlConfig::kSsid);
        std::snprintf(reinterpret_cast<char*>(wifi_config.sta.password),
                      sizeof(wifi_config.sta.password),
                      "%s",
                      WifiControlConfig::kPassword);
        wifi_config.sta.pmf_cfg.capable = true;
        wifi_config.sta.pmf_cfg.required = false;

        err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err == ESP_OK) {
            err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
        }
        if (err == ESP_OK) {
            err = esp_wifi_start();
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Wi-Fi start failed: %s", esp_err_to_name(err));
            return false;
        }

        const EventBits_t bits = xEventGroupWaitBits(wifi_event_group_,
                                                     kWifiConnectedBit,
                                                     pdFALSE,
                                                     pdFALSE,
                                                     kWifiConnectTimeout);
        if ((bits & kWifiConnectedBit) == 0) {
            ESP_LOGW(TAG,
                     "Wi-Fi did not connect within %u ms for hostname=%s",
                     static_cast<unsigned>(pdTICKS_TO_MS(kWifiConnectTimeout)),
                     WifiControlConfig::kNodeName);
        }

        return true;
    }

    void handleWifiEvent(esp_event_base_t event_base, int32_t event_id, void* event_data)
    {
        if (event_base == WIFI_EVENT) {
            if (event_id == WIFI_EVENT_STA_START) {
                ESP_LOGI(TAG, "Wi-Fi STA starting, hostname=%s", WifiControlConfig::kNodeName);
                (void)esp_wifi_connect();
                return;
            }

            if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
                xEventGroupClearBits(wifi_event_group_, kWifiConnectedBit);
                wifi_connected_ = false;
                const auto* disconnected = static_cast<wifi_event_sta_disconnected_t*>(event_data);
                ESP_LOGW(TAG,
                         "Wi-Fi disconnected: reason=%u, retrying...",
                         disconnected ? static_cast<unsigned>(disconnected->reason) : 0u);
                (void)esp_wifi_connect();
                return;
            }
        }

        if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
            const auto* got_ip = static_cast<ip_event_got_ip_t*>(event_data);
            xEventGroupSetBits(wifi_event_group_, kWifiConnectedBit);
            wifi_connected_ = true;
            ESP_LOGI(TAG,
                     "Wi-Fi connected: hostname=%s ip=" IPSTR,
                     WifiControlConfig::kNodeName,
                     IP2STR(&got_ip->ip_info.ip));
        }
    }

    void wifiControlTask()
    {
        while (true) {
            const EventBits_t bits = xEventGroupWaitBits(wifi_event_group_,
                                                         kWifiConnectedBit,
                                                         pdFALSE,
                                                         pdFALSE,
                                                         kWifiControlPollPeriod);
            if ((bits & kWifiConnectedBit) != 0 && !http_server_) {
                (void)startHttpServer();
            }
        }
    }

    esp_err_t handleHttpStatus(httpd_req_t* req)
    {
        return sendHttpStatusResponse(req);
    }

    esp_err_t handleHttpRxStart(httpd_req_t* req)
    {
        const bool ok = dispatchHttpCommand("RX");
        return sendHttpStatusResponse(req, ok ? "200 OK" : "500 Internal Server Error");
    }

    esp_err_t handleHttpTxStart(httpd_req_t* req)
    {
        const bool ok = dispatchHttpCommand("TX");
        return sendHttpStatusResponse(req, ok ? "200 OK" : "500 Internal Server Error");
    }

    esp_err_t handleHttpStop(httpd_req_t* req)
    {
        const bool ok = dispatchHttpCommand("STOP");
        return sendHttpStatusResponse(req, ok ? "200 OK" : "500 Internal Server Error");
    }

    esp_err_t handleHttpChannel(httpd_req_t* req)
    {
        const size_t query_len = httpd_req_get_url_query_len(req);
        if (query_len == 0) {
            return sendHttpJson(req, "{\"error\":\"missing_value\"}", "400 Bad Request");
        }

        std::string query(query_len + 1, '\0');
        if (httpd_req_get_url_query_str(req, query.data(), query.size()) != ESP_OK) {
            return sendHttpJson(req, "{\"error\":\"invalid_query\"}", "400 Bad Request");
        }

        char value_buf[8] = {};
        if (httpd_query_key_value(query.c_str(), "value", value_buf, sizeof(value_buf)) != ESP_OK) {
            return sendHttpJson(req, "{\"error\":\"missing_value\"}", "400 Bad Request");
        }

        uint8_t channel = 0;
        if (!parseUint8Arg(value_buf, 0, 125, channel)) {
            return sendHttpJson(req, "{\"error\":\"invalid_channel\"}", "400 Bad Request");
        }

        std::string command = "CHANNEL ";
        command += value_buf;
        const bool ok = dispatchHttpCommand(command);
        return sendHttpStatusResponse(req, ok ? "200 OK" : "500 Internal Server Error");
    }

    void printPrompt() const
    {
        std::printf("\nrf24> ");
        std::fflush(stdout);
    }

    void printHelp() const
    {
        // Keep the runtime help text close to the command handlers so the
        // console stays self-documenting as commands evolve.
        std::printf(
            "\nCommands:\n"
            "  HELP                 Show this command list\n"
            "  STATUS               Show radio state and selected file\n"
            "  STOP                 Stop any active TX/CW/Morse/RX and return to standby\n"
            "  FILES                List available .u8 tracks in SPIFFS\n"
            "  SELECT <file>        Choose which SPIFFS track TX will send\n"
            "  TX [file]            Start sending the selected or named track\n"
            "  TX LOOP [n|INF] [f]  Repeatedly send the selected or named track\n"
            "  MORSE <text>         Key A-Z/0-9/spaces as Morse; use STOP to abort\n"
            "  RX                   Enter receive/listen mode\n"
            "  STANDBY              Leave RX/CW/sleep and return to standby\n"
            "  SLEEP                Put the radio into sleep mode\n"
            "  WAKE                 Wake the radio back to standby\n"
            "  POWERDOWN            Fully power down the radio\n"
            "  CHANNEL <0-125>      Reinitialize the radio on a new channel\n"
            "  CW START [ch] [0-3]  Start a continuous-wave test on a channel/power level\n"
            "  CW LOOP <on> <off>   Repeat CW bursts; optional [ch] [pwr] [EVERY <loops>]\n"
            "\nPrepare new tracks on the host with the PlatformIO 'Prepare Demo Audio' target\n"
            "or by running: python tools/prepare_demo_audio.py <path-to-song.mp3>\n");

        if (kWirelessControlEnabled) {
            std::printf("  REMOTE <cmd>         Send a short wireless command to a listening peer\n");
        }
    }

    bool takeRadio(TickType_t timeout = portMAX_DELAY)
    {
        // All radio operations flow through one mutual exclusion lock (mutex)
        // so the command handlers and the receive (RX) polling task never talk
        // to the chip simultaneously.
        return xSemaphoreTake(radio_mutex_, timeout) == pdTRUE;
    }

    void giveRadio()
    {
        xSemaphoreGive(radio_mutex_);
    }

    bool takeCommand(TickType_t timeout = portMAX_DELAY)
    {
        return xSemaphoreTake(command_mutex_, timeout) == pdTRUE;
    }

    void giveCommand()
    {
        xSemaphoreGive(command_mutex_);
    }

    bool takeLoop(TickType_t timeout = portMAX_DELAY)
    {
        return xSemaphoreTake(loop_mutex_, timeout) == pdTRUE;
    }

    void giveLoop()
    {
        xSemaphoreGive(loop_mutex_);
    }

    LoopConfig loopSnapshot()
    {
        LoopConfig snapshot;
        if (!takeLoop(pdMS_TO_TICKS(50))) {
            return snapshot;
        }

        snapshot = loop_config_;
        giveLoop();
        return snapshot;
    }

    void clearLoopLocked()
    {
        loop_config_ = LoopConfig{};
    }

    void resetRxSession()
    {
        last_carrier_detected_ = false;
        carrier_event_count_ = 0;
        decoded_rx_packet_count_ = 0;
        raw_rx_packet_count_ = 0;
        sync_gate_.reset();
    }

    bool isLoopActive()
    {
        if (!takeLoop(pdMS_TO_TICKS(50))) {
            return false;
        }

        const bool active = loop_config_.active;
        giveLoop();
        return active;
    }

    bool stopLoopAndWait(TickType_t timeout = kLoopStopTimeout)
    {
        if (!isLoopActive()) {
            loop_stop_requested_ = false;
            return true;
        }

        loop_stop_requested_ = true;
        TickType_t waited = 0;
        while (waited < timeout) {
            if (!isLoopActive()) {
                loop_stop_requested_ = false;
                return true;
            }

            vTaskDelay(kLoopStopPollPeriod);
            waited += kLoopStopPollPeriod;
        }

        return !isLoopActive();
    }

    bool waitForLoopStopOrTimeout(uint32_t duration_ms)
    {
        uint32_t remaining_ms = duration_ms;
        while (remaining_ms > 0) {
            if (loop_stop_requested_) {
                return false;
            }

            const uint32_t slice_ms = std::min<uint32_t>(remaining_ms, 20);
            delayAtLeastMs(slice_ms);
            remaining_ms -= slice_ms;
        }

        return !loop_stop_requested_;
    }

    bool stopCurrentCwIfNeeded()
    {
        if (!takeRadio(pdMS_TO_TICKS(100))) {
            return false;
        }

        bool ok = true;
        if (manager_.status().state == RadioState::CwTest) {
            ok = manager_.stopCw();
        }
        giveRadio();
        return ok;
    }

    bool ensureStandbyLocked()
    {
        // Many commands only make sense from standby. This helper coerces the
        // current radio state back to standby while the mutex is already held.
        //
        // Examples:
        // - a fresh fault or boot state triggers a full re-boot
        // - continuous-wave (CW) mode is stopped
        // - receive (RX) mode is exited
        // - sleep/powerdown is woken back up
        RadioStatus status = manager_.status();
        if (status.state == RadioState::Fault || status.state == RadioState::Boot) {
            return manager_.boot(status.channel);
        }
        if (status.state == RadioState::CwTest) {
            if (!manager_.stopCw()) {
                return false;
            }
            status = manager_.status();
        }
        if (status.state == RadioState::RxListening) {
            if (!manager_.leaveRx()) {
                return false;
            }
            status = manager_.status();
        }
        if (status.state == RadioState::Sleep || status.state == RadioState::PowerDown) {
            if (!manager_.wake()) {
                return false;
            }
        }
        return manager_.status().state == RadioState::Standby;
    }

    bool startRxLocked()
    {
        bool ok = ensureStandbyLocked();
        if (ok) {
            ok = manager_.enterRx();
        }
        if (ok) {
            resetRxSession();
        }
        return ok;
    }

    bool isRemoteCommandAllowed(const std::vector<std::string>& words) const
    {
        if (words.empty()) {
            return false;
        }

        const std::string command = uppercaseCopy(words.front());
        return command == "HELP" ||
               command == "?" ||
               command == "STATUS" ||
               command == "FILES" ||
               command == "LS" ||
               command == "SELECT" ||
               command == "TX" ||
               command == "MORSE" ||
               command == "STOP" ||
               command == "CHANNEL";
    }

    void tryResumeWirelessRx(const char* reason)
    {
        if (!kWirelessControlEnabled || !kWirelessControlAutoRx || isLoopActive()) {
            return;
        }

        if (!takeRadio(pdMS_TO_TICKS(100))) {
            ESP_LOGW(TAG, "%s: radio busy, could not resume RX", reason);
            return;
        }

        const RadioStatus before = manager_.status();
        bool ok = true;
        if (before.state == RadioState::Standby) {
            ok = manager_.enterRx();
            if (ok) {
                resetRxSession();
            }
        }

        const RadioStatus after = manager_.status();
        giveRadio();

        if (before.state == RadioState::Standby) {
            if (ok) {
                ESP_LOGI(TAG, "%s on channel %u",
                         reason,
                         static_cast<unsigned>(after.channel));
            } else {
                ESP_LOGW(TAG, "%s failed, state=%s fault=%d",
                         reason,
                         RadioManager::stateName(after.state),
                         after.last_fault);
            }
        }
    }

    void printStatus()
    {
        // STATUS refreshes the live radio snapshot, then adds a live hasPendingRx()
        // check when receive (RX) mode is active.
        if (!takeRadio(pdMS_TO_TICKS(50))) {
            std::printf("Could not read radio status right now.\n");
            return;
        }

        manager_.refreshSnapshot();
        const bool rx_pending =
            manager_.status().state == RadioState::RxListening ? manager_.hasPendingRx() : false;
        const RadioStatus status = manager_.status();
        const uint32_t carrier_events = carrier_event_count_;
        const uint32_t decoded_packets = decoded_rx_packet_count_;
        const uint32_t raw_packets = raw_rx_packet_count_;
        giveRadio();
        const LoopConfig loop = loopSnapshot();
        const char* irq_state =
            !status.irq_connected ? "disabled" : (status.irq_asserted ? "low" : "high");

        std::printf("State=%s channel=%u ",
                    RadioManager::stateName(status.state),
                    static_cast<unsigned>(status.channel));
        if (status.power_level >= 0) {
            std::printf("power=%d ", status.power_level);
        } else {
            std::printf("power=unknown ");
        }
        std::printf("selected=%s last_status=0x%02X fifo=0x%02X observe=0x%02X irq=%s tx_irq_seen=%s tx_ok=%s tx_timeout=%s rx_len=%u rx_packets=%u rx_audio=%u rx_raw=%u carrier_events=%u fault=%d",
                    selected_track_.c_str(),
                    static_cast<unsigned>(status.last_status),
                    static_cast<unsigned>(status.last_fifo_status),
                    static_cast<unsigned>(status.last_observe_tx),
                    irq_state,
                    status.last_tx_saw_irq ? "true" : "false",
                    status.last_tx_ok ? "true" : "false",
                    status.last_tx_timed_out ? "true" : "false",
                    static_cast<unsigned>(status.last_rx_len),
                    static_cast<unsigned>(status.rx_packets),
                    static_cast<unsigned>(decoded_packets),
                    static_cast<unsigned>(raw_packets),
                    static_cast<unsigned>(carrier_events),
                    status.last_fault);
        if (kWirelessControlEnabled) {
            std::printf(" remote=enabled");
            if (kWirelessControlAutoRx) {
                std::printf(" remote_auto_rx=true");
            }
        }
        if (status.state == RadioState::RxListening) {
            std::printf(" rx_pending=%s rpd=%s",
                        rx_pending ? "true" : "false",
                        status.carrier_detected ? "true" : "false");
        }
        if (!last_morse_text_.empty()) {
            std::printf(" last_morse=\"%s\"", last_morse_text_.c_str());
        }
        if (loop.active) {
            std::printf(" loop=%s", loopModeName(loop.mode));
            if (loop.mode == LoopMode::Tx) {
                std::printf(" loop_track=%s", loop.track_name.c_str());
                if (loop.infinite) {
                    std::printf(" loop_remaining=inf");
                } else {
                    std::printf(" loop_remaining=%u", static_cast<unsigned>(loop.remaining_iterations));
                }
            } else if (loop.mode == LoopMode::Cw) {
                std::printf(" loop_on_ms=%u loop_off_ms=%u loop_power=%u",
                            static_cast<unsigned>(loop.cw_on_ms),
                            static_cast<unsigned>(loop.cw_off_ms),
                            static_cast<unsigned>(loop.power_level));
                if (loop.cw_report_every > 0) {
                    std::printf(" loop_report_every=%u",
                                static_cast<unsigned>(loop.cw_report_every));
                }
            } else if (loop.mode == LoopMode::Morse) {
                std::printf(" loop_text=%s", loop.morse_text.c_str());
            }
            std::printf(" loop_done=%u", static_cast<unsigned>(loop.completed_iterations));
        }
        std::printf("\n");
    }

    bool captureHttpStatus(HttpStatusSnapshot& out)
    {
        if (!takeRadio(pdMS_TO_TICKS(50))) {
            return false;
        }

        manager_.refreshSnapshot();
        const RadioStatus status = manager_.status();
        out.node_name = WifiControlConfig::kNodeName;
        out.hostname = WifiControlConfig::kNodeName;
        out.state_name = RadioManager::stateName(status.state);
        out.channel = status.channel;
        out.tx_ok = status.last_tx_ok;
        out.tx_timeout = status.last_tx_timed_out;
        out.rx_packets = status.rx_packets;
        out.rx_audio = decoded_rx_packet_count_;
        out.rx_raw = raw_rx_packet_count_;
        out.last_fault = status.last_fault;
        giveRadio();
        return true;
    }

    std::string buildStatusJson()
    {
        HttpStatusSnapshot snapshot{};
        if (!captureHttpStatus(snapshot)) {
            snapshot.state_name = "Unavailable";
        }

        std::string json;
        appendFormat(json,
                     "{\"node_name\":\"%s\",\"hostname\":\"%s\",\"state\":\"%s\",\"channel\":%u,"
                     "\"tx_ok\":%s,\"tx_timeout\":%s,\"rx_packets\":%u,"
                     "\"rx_audio\":%u,\"rx_raw\":%u,\"last_fault\":%d}",
                     snapshot.node_name,
                     snapshot.hostname,
                     snapshot.state_name,
                     static_cast<unsigned>(snapshot.channel),
                     snapshot.tx_ok ? "true" : "false",
                     snapshot.tx_timeout ? "true" : "false",
                     static_cast<unsigned>(snapshot.rx_packets),
                     static_cast<unsigned>(snapshot.rx_audio),
                     static_cast<unsigned>(snapshot.rx_raw),
                     snapshot.last_fault);
        return json;
    }

    bool dispatchHttpCommand(const std::string& line)
    {
        return dispatchCommand(line, CommandOrigin::Http);
    }

    bool commandFiles()
    {
        printTrackTable(listTracks(), selected_track_);
        return true;
    }

    bool commandSelect(const std::vector<std::string>& words)
    {
        // SELECT only changes the default file used by later transmit (TX)
        // commands. It
        // does not start transmission immediately.
        if (words.size() < 2) {
            std::printf("Usage: SELECT <file.u8>\n");
            return false;
        }

        TrackInfo track{};
        if (!resolveTrack(words[1], track)) {
            std::printf("Track '%s' was not found in SPIFFS.\n", words[1].c_str());
            return false;
        }

        selected_track_ = track.name;
        std::printf("Selected %s (%.1f s)\n", selected_track_.c_str(), durationSeconds(track.bytes));
        return true;
    }

    bool commandTx(const std::vector<std::string>& words, CommandOrigin origin)
    {
        if (words.size() >= 2) {
            const std::string action = uppercaseCopy(words[1]);
            if (action == "STOP") {
                std::printf("Use STOP to abort TX.\n");
                return false;
            }

            if (action == "LOOP") {
                if (words.size() > 4) {
                    std::printf("Usage: TX LOOP [count|INF] [file.u8]\n");
                    return false;
                }

                bool infinite = true;
                uint32_t loop_count = 0;
                std::string request = selected_track_;

                if (words.size() >= 3) {
                    if (parseLoopCountToken(words[2], infinite, loop_count)) {
                        if (words.size() >= 4) {
                            request = words[3];
                        }
                    } else {
                        request = words[2];
                    }
                }

                TrackInfo track{};
                if (!resolveTrack(request, track)) {
                    std::printf("Track '%s' was not found in SPIFFS.\n", request.c_str());
                    return false;
                }

                if (!stopLoopAndWait()) {
                    std::printf("Could not stop the current loop cleanly.\n");
                    return false;
                }

                if (!takeLoop()) {
                    std::printf("Could not start TX loop right now.\n");
                    return false;
                }

                selected_track_ = track.name;
                loop_stop_requested_ = false;
                loop_config_ = LoopConfig{};
                loop_config_.mode = LoopMode::Tx;
                loop_config_.active = true;
                loop_config_.infinite = infinite;
                loop_config_.restore_rx_after_completion =
                    origin == CommandOrigin::Remote && kWirelessControlEnabled && kWirelessControlAutoRx;
                loop_config_.remaining_iterations = loop_count;
                loop_config_.track_name = track.name;
                giveLoop();

                if (infinite) {
                    std::printf("TX loop active for %s (infinite)\n", track.name.c_str());
                } else {
                    std::printf("TX loop active for %s (%u passes)\n",
                                track.name.c_str(),
                                static_cast<unsigned>(loop_count));
                }
                return true;
            }
        }

        // Transmit (TX) either uses the explicitly requested file or the
        // currently
        // selected default track.
        const std::string request = words.size() >= 2 ? words[1] : selected_track_;
        TrackInfo track{};
        if (!resolveTrack(request, track)) {
            std::printf("Track '%s' was not found in SPIFFS.\n", request.c_str());
            return false;
        }

        if (!stopLoopAndWait()) {
            std::printf("Could not stop the current loop cleanly.\n");
            return false;
        }

        if (!takeLoop()) {
            std::printf("Could not start TX right now.\n");
            return false;
        }

        selected_track_ = track.name;
        loop_stop_requested_ = false;
        loop_config_ = LoopConfig{};
        loop_config_.mode = LoopMode::Tx;
        loop_config_.active = true;
        loop_config_.infinite = false;
        loop_config_.restore_rx_after_completion =
            origin == CommandOrigin::Remote && kWirelessControlEnabled && kWirelessControlAutoRx;
        loop_config_.remaining_iterations = 1;
        loop_config_.track_name = track.name;
        giveLoop();

        std::printf("TX started for %s. Use STOP to abort.\n", track.name.c_str());
        return true;
    }

    bool commandRemote(const std::string& line)
    {
        if (!kWirelessControlEnabled) {
            std::printf("REMOTE is disabled in this build. Use the Wi-Fi HTTP endpoints instead.\n");
            return false;
        }

        const size_t separator = line.find_first_of(" \t");
        if (separator == std::string::npos) {
            std::printf("Usage: REMOTE <command...>\n");
            return false;
        }

        const std::string request = trimAscii(line.substr(separator + 1));
        if (request.empty()) {
            std::printf("Usage: REMOTE <command...>\n");
            return false;
        }

        const std::vector<std::string> request_words = splitWords(request);
        if (!isRemoteCommandAllowed(request_words)) {
            std::printf("Remote commands support HELP, STATUS, FILES, SELECT, TX, MORSE, STOP, and CHANNEL.\n");
            return false;
        }

        uint8_t packet[AudioPacket::kPacketBytes] = {};
        size_t packet_len = 0;
        if (!StreamSync::encodeRemoteCommand(request.c_str(), request.size(), packet, packet_len)) {
            std::printf("Remote command is too long or has non-ASCII bytes. Limit is %u characters.\n",
                        static_cast<unsigned>(StreamSync::kRemoteCommandMaxBytes));
            return false;
        }

        if (!takeRadio()) {
            std::printf("Radio is busy.\n");
            return false;
        }

        bool ok = ensureStandbyLocked();
        if (ok) {
            ok = manager_.sendPayload(packet, packet_len);
        }

        const RadioStatus status = manager_.status();
        giveRadio();

        if (!ok) {
            std::printf("Could not send remote command. state=%s fault=%d\n",
                        RadioManager::stateName(status.state),
                        status.last_fault);
            return false;
        }

        tryResumeWirelessRx("Remote command transmit complete");
        std::printf("Sent remote command: %s\n", request.c_str());
        return true;
    }

    bool keyMorseEventsLocked(const std::vector<KeyEvent>& events,
                              uint8_t channel,
                              uint8_t power_level,
                              const volatile bool* stop_requested = nullptr,
                              std::string_view live_render = {})
    {
        if (events.empty()) {
            return false;
        }

        const uint8_t rf_power_bits = static_cast<uint8_t>(power_level << 1);
        size_t render_cursor = 0;
        const bool show_preview = !live_render.empty();
        const auto flushPreviewSeparators = [&]() {
            bool printed = false;
            while (render_cursor < live_render.size()) {
                const char ch = live_render[render_cursor];
                if (ch != ' ' && ch != '/') {
                    break;
                }

                std::putchar(ch);
                ++render_cursor;
                printed = true;
            }

            if (printed) {
                std::fflush(stdout);
            }
        };
        const auto flushPreviewSymbol = [&]() {
            flushPreviewSeparators();
            while (render_cursor < live_render.size()) {
                const char ch = live_render[render_cursor];
                if (ch != '.' && ch != '-') {
                    ++render_cursor;
                    continue;
                }

                std::putchar(ch);
                ++render_cursor;
                std::fflush(stdout);
                return;
            }
        };
        const auto finishPreview = [&]() {
            if (!show_preview) {
                return;
            }

            flushPreviewSeparators();
            std::putchar('\n');
            std::fflush(stdout);
        };

        if (show_preview) {
            std::printf("Morse TX: ");
            std::fflush(stdout);
        }

        for (const KeyEvent& event : events) {
            if (stop_requested && *stop_requested) {
                if (manager_.status().state == RadioState::CwTest) {
                    manager_.stopCw();
                }
                finishPreview();
                return false;
            }

            bool ok = true;

            if (event.key_down) {
                if (show_preview) {
                    flushPreviewSymbol();
                }
                ok = manager_.startCw(channel, rf_power_bits);
            } else if (manager_.status().state == RadioState::CwTest) {
                if (show_preview) {
                    flushPreviewSeparators();
                }
                ok = manager_.stopCw();
            }

            if (!ok) {
                if (manager_.status().state == RadioState::CwTest) {
                    manager_.stopCw();
                }
                finishPreview();
                return false;
            }

            if (stop_requested) {
                if (!waitForLoopStopOrTimeout(event.duration_ms)) {
                    if (manager_.status().state == RadioState::CwTest) {
                        manager_.stopCw();
                    }
                    finishPreview();
                    return false;
                }
            } else {
                delayAtLeastMs(event.duration_ms);
            }
        }

        if (manager_.status().state == RadioState::CwTest) {
            const bool ok = manager_.stopCw();
            finishPreview();
            return ok;
        }

        finishPreview();
        return manager_.status().state == RadioState::Standby;
    }

    bool commandMorse(const std::string& line, CommandOrigin origin)
    {
        const size_t separator = line.find_first_of(" \t");
        if (separator == std::string::npos) {
            std::printf("Usage: MORSE <text>\n");
            return false;
        }

        const std::string text = trimAscii(line.substr(separator + 1));
        if (text.empty()) {
            std::printf("Usage: MORSE <text>\n");
            return false;
        }

        const ValidationResult dot_result = Validation::dotTimeMs(kDefaultMorseDotMs);
        if (!dot_result.ok) {
            std::printf("Morse timing error: %s\n", dot_result.message);
            return false;
        }

        const std::vector<KeyEvent> events = Morse::encode(text, kDefaultMorseDotMs);
        if (events.empty()) {
            std::printf("Message had no Morse-supported characters. Use A-Z, 0-9, and spaces.\n");
            return false;
        }

        if (!stopLoopAndWait()) {
            std::printf("Could not stop the current loop cleanly.\n");
            return false;
        }

        uint8_t channel = 76;
        if (!takeRadio()) {
            std::printf("Radio is busy.\n");
            return false;
        }
        channel = manager_.status().channel;
        giveRadio();

        if (!takeLoop()) {
            std::printf("Could not start Morse right now.\n");
            return false;
        }

        loop_stop_requested_ = false;
        loop_config_ = LoopConfig{};
        loop_config_.mode = LoopMode::Morse;
        loop_config_.active = true;
        loop_config_.infinite = false;
        loop_config_.restore_rx_after_completion =
            origin == CommandOrigin::Remote && kWirelessControlEnabled && kWirelessControlAutoRx;
        loop_config_.remaining_iterations = 1;
        loop_config_.morse_text = text;
        loop_config_.channel = channel;
        loop_config_.power_level = kDefaultMorsePowerLevel;
        last_morse_text_ = text;
        giveLoop();

        std::printf("Morse started on channel %u at dot=%u ms. Use STOP to abort.\n",
                    static_cast<unsigned>(channel),
                    static_cast<unsigned>(kDefaultMorseDotMs));
        return true;
    }

    bool commandStop()
    {
        const LoopConfig loop = loopSnapshot();

        if (!stopLoopAndWait()) {
            std::printf("Could not stop the current loop cleanly.\n");
            return false;
        }

        if (!takeRadio()) {
            std::printf("Radio is busy.\n");
            return false;
        }

        const RadioStatus before = manager_.status();
        const bool ok = ensureStandbyLocked();
        const RadioStatus after = manager_.status();
        giveRadio();

        if (!ok) {
            std::printf("Could not stop activity. state=%s fault=%d\n",
                        RadioManager::stateName(after.state),
                        after.last_fault);
            return false;
        }

        const bool had_activity =
            loop.active ||
            before.state == RadioState::Boot ||
            before.state == RadioState::RxListening ||
            before.state == RadioState::CwTest ||
            before.state == RadioState::Sleep ||
            before.state == RadioState::PowerDown ||
            before.state == RadioState::TxBusy ||
            before.state == RadioState::Fault;

        if (had_activity) {
            std::printf("Stop complete. Radio is in standby.\n");
        } else {
            std::printf("Radio is already in standby.\n");
        }

        return true;
    }

    bool commandRx()
    {
        // Receive (RX) is a persistent mode rather than a one-shot action. The
        // background
        // rxTask() is what actually polls for and logs packets afterward.
        if (!stopLoopAndWait()) {
            std::printf("Could not stop the current loop cleanly.\n");
            return false;
        }

        if (!takeRadio()) {
            std::printf("Radio is busy.\n");
            return false;
        }

        const bool ok = startRxLocked();

        const RadioStatus status = manager_.status();
        giveRadio();

        if (!ok) {
            std::printf("Could not enter RX mode. state=%s fault=%d\n",
                        RadioManager::stateName(status.state),
                        status.last_fault);
            return false;
        }

        std::printf("RX listening on channel %u\n", static_cast<unsigned>(status.channel));
        return true;
    }

    bool commandStandby()
    {
        // STANDBY is the "normalize state" command for the operator.
        if (!stopLoopAndWait()) {
            std::printf("Could not stop the current loop cleanly.\n");
            return false;
        }

        if (!takeRadio()) {
            std::printf("Radio is busy.\n");
            return false;
        }

        const bool ok = ensureStandbyLocked();
        const RadioStatus status = manager_.status();
        giveRadio();

        if (!ok) {
            std::printf("Could not reach standby. state=%s fault=%d\n",
                        RadioManager::stateName(status.state),
                        status.last_fault);
            return false;
        }

        std::printf("Radio is in standby on channel %u\n", static_cast<unsigned>(status.channel));
        return true;
    }

    bool commandSleep()
    {
        // Sleep keeps the radio configuration but requests the chip's lower
        // power state.
        if (!stopLoopAndWait()) {
            std::printf("Could not stop the current loop cleanly.\n");
            return false;
        }

        if (!takeRadio()) {
            std::printf("Radio is busy.\n");
            return false;
        }

        bool ok = ensureStandbyLocked();
        if (ok) {
            ok = manager_.sleep();
        }

        const RadioStatus status = manager_.status();
        giveRadio();

        if (!ok) {
            std::printf("Could not enter sleep. state=%s fault=%d\n",
                        RadioManager::stateName(status.state),
                        status.last_fault);
            return false;
        }

        std::printf("Radio is sleeping.\n");
        return true;
    }

    bool commandWake()
    {
        // WAKE simply routes through ensureStandbyLocked(), which knows how to
        // bring sleep or power-down states back to standby.
        if (!stopLoopAndWait()) {
            std::printf("Could not stop the current loop cleanly.\n");
            return false;
        }

        if (!takeRadio()) {
            std::printf("Radio is busy.\n");
            return false;
        }

        const bool ok = ensureStandbyLocked();
        const RadioStatus status = manager_.status();
        giveRadio();

        if (!ok) {
            std::printf("Could not wake radio. state=%s fault=%d\n",
                        RadioManager::stateName(status.state),
                        status.last_fault);
            return false;
        }

        std::printf("Radio is awake in standby.\n");
        return true;
    }

    bool commandPowerDown()
    {
        // Power-down is a stronger operator action than sleep, but the console
        // still treats it as another coarse state transition.
        if (!stopLoopAndWait()) {
            std::printf("Could not stop the current loop cleanly.\n");
            return false;
        }

        if (!takeRadio()) {
            std::printf("Radio is busy.\n");
            return false;
        }

        bool ok = ensureStandbyLocked();
        if (ok) {
            ok = manager_.powerDown();
        }

        const RadioStatus status = manager_.status();
        giveRadio();

        if (!ok) {
            std::printf("Could not power down radio. state=%s fault=%d\n",
                        RadioManager::stateName(status.state),
                        status.last_fault);
            return false;
        }

        std::printf("Radio is powered down.\n");
        return true;
    }

    bool commandChannel(const std::vector<std::string>& words)
    {
        // Changing channels is implemented as a full re-boot of the radio so
        // all configuration returns to the known defaults for that channel.
        if (words.size() < 2) {
            std::printf("Usage: CHANNEL <0-125>\n");
            return false;
        }

        uint8_t channel = 0;
        if (!parseUint8Arg(words[1], 0, 125, channel)) {
            std::printf("Channel must be in the range 0-125.\n");
            return false;
        }

        if (!stopLoopAndWait()) {
            std::printf("Could not stop the current loop cleanly.\n");
            return false;
        }

        if (!takeRadio()) {
            std::printf("Radio is busy.\n");
            return false;
        }

        const bool ok = manager_.boot(channel);
        const RadioStatus status = manager_.status();
        giveRadio();

        if (!ok) {
            std::printf("Could not switch channel. state=%s fault=%d\n",
                        RadioManager::stateName(status.state),
                        status.last_fault);
            return false;
        }

        std::printf("Radio reinitialized on channel %u\n", static_cast<unsigned>(status.channel));
        return true;
    }

    bool commandCw(const std::vector<std::string>& words)
    {
        // Continuous-wave (CW) exposes a simple radio-frequency (RF) test mode
        // for verifying channel/power output without sending normal packet
        // payloads.
        if (words.size() < 2) {
            std::printf("Usage: CW START [channel] [power0-3] | CW LOOP <on_ms> <off_ms> [channel] [power0-3] [EVERY <loops>]\n");
            return false;
        }

        const std::string action = uppercaseCopy(words[1]);
        if (action == "STOP") {
            return commandStop();
        }

        if (action == "LOOP") {
            if (words.size() < 4) {
                std::printf("Usage: CW LOOP <on_ms> <off_ms> [channel] [power0-3] [EVERY <loops>]\n");
                return false;
            }

            uint32_t on_ms = 0;
            uint32_t off_ms = 0;
            if (!parseUint32Arg(words[2], 1, UINT32_MAX, on_ms) ||
                !parseUint32Arg(words[3], 1, UINT32_MAX, off_ms)) {
                std::printf("CW loop timings must be positive millisecond values.\n");
                return false;
            }

            if (!Validation::cwDurationMs(on_ms).ok || !Validation::cwDurationMs(off_ms).ok) {
                std::printf("CW loop timings must be greater than zero.\n");
                return false;
            }

            uint8_t channel = manager_.status().channel;
            uint8_t power_level = 3;
            uint32_t report_every = 0;
            bool parsed_channel = false;
            bool parsed_power = false;

            for (size_t i = 4; i < words.size(); ++i) {
                const std::string token = uppercaseCopy(words[i]);
                if (token == "EVERY") {
                    if (i + 1 >= words.size() ||
                        !parseUint32Arg(words[i + 1], 1, UINT32_MAX, report_every)) {
                        std::printf("CW loop report interval must be a positive loop count.\n");
                        return false;
                    }
                    ++i;
                    continue;
                }

                if (!parsed_channel) {
                    if (!parseUint8Arg(words[i], 0, 125, channel)) {
                        std::printf("CW channel must be in the range 0-125.\n");
                        return false;
                    }
                    parsed_channel = true;
                    continue;
                }

                if (!parsed_power) {
                    if (!parseUint8Arg(words[i], 0, 3, power_level)) {
                        std::printf("CW power level must be in the range 0-3.\n");
                        return false;
                    }
                    parsed_power = true;
                    continue;
                }

                std::printf("Usage: CW LOOP <on_ms> <off_ms> [channel] [power0-3] [EVERY <loops>]\n");
                return false;
            }

            if (!stopLoopAndWait()) {
                std::printf("Could not stop the current loop cleanly.\n");
                return false;
            }

            if (!takeLoop()) {
                std::printf("Could not start CW loop right now.\n");
                return false;
            }

            loop_stop_requested_ = false;
            loop_config_ = LoopConfig{};
            loop_config_.mode = LoopMode::Cw;
            loop_config_.active = true;
            loop_config_.infinite = true;
            loop_config_.cw_on_ms = on_ms;
            loop_config_.cw_off_ms = off_ms;
            loop_config_.cw_report_every = report_every;
            loop_config_.channel = channel;
            loop_config_.power_level = power_level;
            giveLoop();

            std::printf("CW mode active on channel %u at power level %u (%u ms on, %u ms off)",
                        static_cast<unsigned>(channel),
                        static_cast<unsigned>(power_level),
                        static_cast<unsigned>(on_ms),
                        static_cast<unsigned>(off_ms));
            if (report_every > 0) {
                std::printf(", reporting every %u loops", static_cast<unsigned>(report_every));
            }
            std::printf(". Use STOP to abort.\n");
            return true;
        }

        if (action != "START") {
            std::printf("Usage: CW START [channel] [power0-3] | CW LOOP <on_ms> <off_ms> [channel] [power0-3] [EVERY <loops>]\n");
            return false;
        }

        if (!stopLoopAndWait()) {
            std::printf("Could not stop the current loop cleanly.\n");
            return false;
        }

        if (!takeRadio()) {
            std::printf("Radio is busy.\n");
            return false;
        }

        // Defaults follow the current configured channel and maximum power so
        // a bare "CW START" is convenient during bench testing.
        uint8_t channel = manager_.status().channel;
        uint8_t power_level = 3;

        if (words.size() >= 3 && !parseUint8Arg(words[2], 0, 125, channel)) {
            giveRadio();
            std::printf("CW channel must be in the range 0-125.\n");
            return false;
        }

        if (words.size() >= 4 && !parseUint8Arg(words[3], 0, 3, power_level)) {
            giveRadio();
            std::printf("CW power level must be in the range 0-3.\n");
            return false;
        }

        bool ok = ensureStandbyLocked();
        if (ok) {
            ok = manager_.startCw(channel, static_cast<uint8_t>(power_level << 1));
        }

        const RadioStatus status = manager_.status();
        giveRadio();

        if (!ok) {
            std::printf("Could not start CW mode. state=%s fault=%d\n",
                        RadioManager::stateName(status.state),
                        status.last_fault);
            return false;
        }

        std::printf("CW mode active on channel %u at power level %u. Use STOP to abort.\n",
                    static_cast<unsigned>(status.channel),
                    static_cast<unsigned>(power_level));
        return true;
    }

    void printCwLoopReport(const LoopConfig& loop, uint32_t completed_iterations)
    {
        if (!takeRadio(pdMS_TO_TICKS(50))) {
            return;
        }

        manager_.refreshSnapshot();
        const RadioStatus status = manager_.status();
        giveRadio();

        const unsigned channel = static_cast<unsigned>(status.channel);
        const unsigned power_level = status.power_level >= 0
            ? static_cast<unsigned>(status.power_level)
            : static_cast<unsigned>(loop.power_level);

        std::printf("CW mode active on channel %u at power level %u (%u ms on, %u ms off, loop %u)\n",
                    channel,
                    power_level,
                    static_cast<unsigned>(loop.cw_on_ms),
                    static_cast<unsigned>(loop.cw_off_ms),
                    static_cast<unsigned>(completed_iterations));
        std::fflush(stdout);
    }

    void runTxLoopIteration(const LoopConfig& loop)
    {
        if (!takeRadio()) {
            return;
        }

        bool ok = ensureStandbyLocked();
        RadioStatus status = manager_.status();
        if (ok) {
            const std::string path = buildTrackPath(loop.track_name);
            ok = sendU8Song(manager_, path.c_str(), &loop_stop_requested_);
            status = manager_.status();
        }
        giveRadio();

        const bool stopped = loop_stop_requested_;
        bool restore_rx = false;
        if (!takeLoop(pdMS_TO_TICKS(50))) {
            return;
        }

        if (loop_config_.active && loop_config_.mode == LoopMode::Tx) {
            if (ok) {
                ++loop_config_.completed_iterations;
                if (!loop_config_.infinite && loop_config_.remaining_iterations > 0) {
                    --loop_config_.remaining_iterations;
                }
            }

            const bool finished = ok && !loop_config_.infinite && loop_config_.remaining_iterations == 0;
            if (stopped || !ok || finished) {
                restore_rx = loop_config_.restore_rx_after_completion;
                clearLoopLocked();
            }
        }
        giveLoop();

        if (ok) {
            ESP_LOGI(TAG, "TX pass complete for %s", loop.track_name.c_str());
        } else if (stopped) {
            ESP_LOGI(TAG, "TX stopped for %s", loop.track_name.c_str());
        }

        if (!ok && !stopped) {
            ESP_LOGW(TAG, "TX loop stopped, state=%s fault=%d",
                     RadioManager::stateName(status.state),
                     status.last_fault);
        }

        if (restore_rx) {
            tryResumeWirelessRx("Remote control RX resumed after TX");
        }
    }

    void runCwLoopCycle(const LoopConfig& loop)
    {
        if (!takeRadio()) {
            return;
        }

        bool ok = ensureStandbyLocked();
        RadioStatus status = manager_.status();
        if (ok) {
            ok = manager_.startCw(loop.channel, static_cast<uint8_t>(loop.power_level << 1));
            status = manager_.status();
        }
        giveRadio();

        if (!ok) {
            if (takeLoop(pdMS_TO_TICKS(50))) {
                if (loop_config_.active && loop_config_.mode == LoopMode::Cw) {
                    clearLoopLocked();
                }
                giveLoop();
            }
            ESP_LOGW(TAG, "CW loop stopped, state=%s fault=%d",
                     RadioManager::stateName(status.state),
                     status.last_fault);
            return;
        }

        waitForLoopStopOrTimeout(loop.cw_on_ms);

        if (!stopCurrentCwIfNeeded()) {
            return;
        }

        const bool stopped = loop_stop_requested_;
        bool should_report = false;
        uint32_t completed_iterations = 0;
        if (takeLoop(pdMS_TO_TICKS(50))) {
            if (loop_config_.active && loop_config_.mode == LoopMode::Cw) {
                ++loop_config_.completed_iterations;
                completed_iterations = loop_config_.completed_iterations;
                should_report =
                    !stopped &&
                    loop.cw_report_every > 0 &&
                    completed_iterations > 0 &&
                    (completed_iterations % loop.cw_report_every) == 0;
                if (stopped) {
                    clearLoopLocked();
                }
            }
            giveLoop();
        }

        if (stopped) {
            return;
        }

        if (should_report) {
            printCwLoopReport(loop, completed_iterations);
        }

        waitForLoopStopOrTimeout(loop.cw_off_ms);

        if (loop_stop_requested_ && takeLoop(pdMS_TO_TICKS(50))) {
            if (loop_config_.active && loop_config_.mode == LoopMode::Cw) {
                clearLoopLocked();
            }
            giveLoop();
        }
    }

    void runMorseIteration(const LoopConfig& loop)
    {
        const std::vector<KeyEvent> events = Morse::encode(loop.morse_text, kDefaultMorseDotMs);
        const std::string morse_line = Morse::render(loop.morse_text);
        if (events.empty()) {
            if (takeLoop(pdMS_TO_TICKS(50))) {
                if (loop_config_.active && loop_config_.mode == LoopMode::Morse) {
                    clearLoopLocked();
                }
                giveLoop();
            }
            return;
        }

        if (!takeRadio()) {
            return;
        }

        bool ok = ensureStandbyLocked();
        RadioStatus status = manager_.status();
        if (ok) {
            ESP_LOGI(TAG, "Starting Morse on channel %u: %s",
                     static_cast<unsigned>(loop.channel),
                     loop.morse_text.c_str());
            ok = keyMorseEventsLocked(events,
                                      loop.channel,
                                      loop.power_level,
                                      &loop_stop_requested_,
                                      morse_line);
            status = manager_.status();
        }
        giveRadio();

        const bool stopped = loop_stop_requested_;
        bool restore_rx = false;
        if (takeLoop(pdMS_TO_TICKS(50))) {
            if (loop_config_.active && loop_config_.mode == LoopMode::Morse) {
                if (ok) {
                    ++loop_config_.completed_iterations;
                    if (!loop_config_.infinite && loop_config_.remaining_iterations > 0) {
                        --loop_config_.remaining_iterations;
                    }
                }

                restore_rx = loop_config_.restore_rx_after_completion;
                clearLoopLocked();
            }
            giveLoop();
        }

        if (ok) {
            ESP_LOGI(TAG, "Morse complete: %s", loop.morse_text.c_str());
        } else if (stopped) {
            ESP_LOGI(TAG, "Morse stopped: %s", loop.morse_text.c_str());
        } else {
            ESP_LOGW(TAG, "Morse stopped, state=%s fault=%d",
                     RadioManager::stateName(status.state),
                     status.last_fault);
        }

        if (restore_rx) {
            tryResumeWirelessRx("Remote control RX resumed after Morse");
        }
    }

    void loopTask()
    {
        while (true) {
            const LoopConfig loop = loopSnapshot();
            if (!loop.active) {
                vTaskDelay(kLoopWorkerPeriod);
                continue;
            }

            if (loop.mode == LoopMode::Tx) {
                runTxLoopIteration(loop);
                continue;
            }

            if (loop.mode == LoopMode::Cw) {
                runCwLoopCycle(loop);
                continue;
            }

            if (loop.mode == LoopMode::Morse) {
                runMorseIteration(loop);
                continue;
            }

            vTaskDelay(kLoopWorkerPeriod);
        }
    }

    void logRxPacket(const uint8_t* payload, size_t len)
    {
        AudioPacket::Header header{};
        const uint8_t* audio = nullptr;

        switch (sync_gate_.accept(payload, len, &header, &audio)) {
            case StreamSync::ReceiverGate::Action::StartAccepted:
                ESP_LOGI(TAG, "RX START stream=%u",
                         static_cast<unsigned>(sync_gate_.currentStreamId()));
                return;
            case StreamSync::ReceiverGate::Action::StopAccepted:
                ESP_LOGI(TAG, "RX STOP");
                return;

            case StreamSync::ReceiverGate::Action::AudioAccepted:
                ++decoded_rx_packet_count_;
                ESP_LOGI(TAG,
                         "RX packet stream=%u seq=%u audio=%u flags=0x%02X",
                         static_cast<unsigned>(sync_gate_.currentStreamId()),
                         static_cast<unsigned>(header.sequence),
                         static_cast<unsigned>(header.audio_len),
                         static_cast<unsigned>(header.flags));
                return;
            case StreamSync::ReceiverGate::Action::Ignore:
                ++raw_rx_packet_count_;
                ESP_LOGI(TAG, "RX packet ignored while waiting for sync");
                return;

            case StreamSync::ReceiverGate::Action::Invalid:
            default:
                ++raw_rx_packet_count_;
                ESP_LOGI(TAG, "RX payload len=%u (unrecognized frame)",
                         static_cast<unsigned>(len));
                return;
        }
    }

    void rxTask()
    {
        // This background task is intentionally conservative:
        // - it tries to grab the mutex briefly
        // - it only touches the radio when receive (RX) mode is active
        // - it drains any queued packets before releasing the radio again
        // - it logs received packets but does not otherwise mutate app state
        std::array<uint8_t, AudioPacket::kPacketBytes> payload{};

        while (true) {
            std::string remote_command;

            if (takeRadio(pdMS_TO_TICKS(10))) {
                if (manager_.status().state == RadioState::RxListening) {
                    manager_.refreshSnapshot();
                    const RadioStatus snapshot = manager_.status();

                    if (snapshot.carrier_detected && !last_carrier_detected_) {
                        ++carrier_event_count_;
                        ESP_LOGI(TAG, "Carrier detect (RPD) high on channel %u",
                                 static_cast<unsigned>(snapshot.channel));
                    }
                    last_carrier_detected_ = snapshot.carrier_detected;

                    while (manager_.hasPendingRx()) {
                        size_t out_len = 0;
                        if (manager_.receivePayload(payload.data(), payload.size(), out_len)) {
                            std::string_view remote_view;
                            if (kWirelessControlEnabled &&
                                StreamSync::decodeRemoteCommand(payload.data(), out_len, remote_view)) {
                                remote_command.assign(remote_view.data(), remote_view.size());
                                break;
                            }

                            logRxPacket(payload.data(), out_len);
                        } else {
                            const RadioStatus status = manager_.status();
                            ESP_LOGW(TAG, "RX read failed, state=%s fault=%d",
                                     RadioManager::stateName(status.state),
                                     status.last_fault);
                            break;
                        }
                    }
                } else {
                    last_carrier_detected_ = false;
                }
                giveRadio();
            }

            if (!remote_command.empty()) {
                ESP_LOGI(TAG, "RX remote command: %s", remote_command.c_str());
                dispatchCommand(remote_command, CommandOrigin::Remote);
            }

            vTaskDelay(kRxPollPeriod);
        }
    }

    bool handleCommand(const std::string& line, CommandOrigin origin)
    {
        // Command dispatch is a simple keyword router. Each handler owns its
        // own validation and user-facing error messages.
        const std::vector<std::string> words = splitWords(line);
        if (words.empty()) {
            return true;
        }

        if (origin == CommandOrigin::Remote && !isRemoteCommandAllowed(words)) {
            std::printf("Remote command '%s' is not supported.\n", words.front().c_str());
            return false;
        }

        const std::string command = uppercaseCopy(words.front());
        if (command == "HELP" || command == "?") {
            printHelp();
            return true;
        }
        if (command == "STOP") {
            return commandStop();
        }
        if (command == "STATUS") {
            printStatus();
            return true;
        }
        if (command == "FILES" || command == "LS") {
            return commandFiles();
        }
        if (command == "SELECT") {
            return commandSelect(words);
        }
        if (command == "TX") {
            return commandTx(words, origin);
        }
        if (command == "MORSE") {
            return commandMorse(line, origin);
        }
        if (command == "REMOTE") {
            return commandRemote(line);
        }
        if (command == "RX") {
            return commandRx();
        }
        if (command == "STANDBY") {
            return commandStandby();
        }
        if (command == "SLEEP") {
            return commandSleep();
        }
        if (command == "WAKE") {
            return commandWake();
        }
        if (command == "POWERDOWN") {
            return commandPowerDown();
        }
        if (command == "CHANNEL") {
            return commandChannel(words);
        }
        if (command == "CW") {
            return commandCw(words);
        }

        std::printf("Unknown command '%s'. Type HELP for the list.\n", words.front().c_str());
        return false;
    }

    bool dispatchCommand(const std::string& line, CommandOrigin origin)
    {
        if (!takeCommand(pdMS_TO_TICKS(500))) {
            if (origin == CommandOrigin::Remote) {
                ESP_LOGW(TAG, "Dropping remote command while another command is active: %s", line.c_str());
            } else {
                std::printf("Command system is busy.\n");
            }
            return false;
        }

        const bool ok = handleCommand(line, origin);
        if (ok && origin == CommandOrigin::Remote) {
            tryResumeWirelessRx("Remote control RX resumed");
        }
        giveCommand();
        return ok;
    }

    RadioManager& manager_;
    SemaphoreHandle_t radio_mutex_ = nullptr;  // Serializes all radio access.
    TaskHandle_t rx_task_ = nullptr;           // Background receive (RX) polling task.
    SemaphoreHandle_t loop_mutex_ = nullptr;   // Guards background TX/CW loop configuration.
    TaskHandle_t loop_task_ = nullptr;         // Background TX/CW loop worker.
    TaskHandle_t wifi_control_task_ = nullptr; // Starts HTTP control off the event-task stack.
    SemaphoreHandle_t command_mutex_ = nullptr;  // Serializes local and remote command dispatch.
    std::string selected_track_ = kDefaultTrack;  // Default file used by transmit (TX).
    std::string last_morse_text_;              // Most recent MORSE text for STATUS output.
    bool last_carrier_detected_ = false;      // Edge detector for RPD logging while in RX.
    uint32_t carrier_event_count_ = 0;        // Number of distinct RPD-high events seen while listening.
    StreamSync::ReceiverGate sync_gate_{};    // RX stream gate for START/STOP/audio synchronization.
    uint32_t decoded_rx_packet_count_ = 0;    // Payloads that matched the AudioPacket format.
    uint32_t raw_rx_packet_count_ = 0;        // Payloads that were received but did not match AudioPacket.
    LoopConfig loop_config_{};
    volatile bool loop_stop_requested_ = false;
    EventGroupHandle_t wifi_event_group_ = nullptr;
    esp_netif_t* wifi_netif_ = nullptr;
    httpd_handle_t http_server_ = nullptr;
    esp_event_handler_instance_t wifi_event_handler_ = nullptr;
    esp_event_handler_instance_t ip_event_handler_ = nullptr;
    bool wifi_connected_ = false;
};
}  // namespace

extern "C" void app_main(void);

void app_main(void)
{
    // app_main is the firmware entry point the Espressif IoT Development
    // Framework (ESP-IDF) calls after the operating system (OS) and
    // drivers are ready. Everything else is built from the default pin config
    // declared in Esp32Nrf24Config.
    Esp32Nrf24Config config{};
    ESP_LOGI(TAG,
             "nRF24 pinset=%s pins: SCK=%d MISO=%d MOSI=%d CE=%d CSN=%d IRQ=%d",
             NRF24_PINSET_NAME,
             static_cast<int>(config.sck_pin),
             static_cast<int>(config.miso_pin),
             static_cast<int>(config.mosi_pin),
             static_cast<int>(config.ce_pin),
             static_cast<int>(config.csn_pin),
             static_cast<int>(config.irq_pin));

    Esp32Nrf24Hal hal(config);
    Nrf24 radio(hal);
    RadioManager manager(radio);
    DemoConsoleApp app(manager);

    if (!app.initialize()) {
        // Keep the firmware alive after initialization failure so the serial
        // monitor can still show the error instead of crashing or reboot-looping.
        ESP_LOGE(TAG, "Demo app initialization failed.");
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    app.run();
}
