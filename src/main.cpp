#include <algorithm>
#include <array>
#include <cctype>
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
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "audio_packet.hpp"
#include "esp32_nrf24_hal.hpp"
#include "nrf24.hpp"
#include "radio_manager.hpp"

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
// Packet rate is derived from the packet payload budget.
constexpr uint32_t kPacketsPerSecond =
    (kAudioBytesPerSecond + AudioPacket::kAudioBytesPerPacket - 1) /
    AudioPacket::kAudioBytesPerPacket;
// Rough over-the-air bitrate including the packet header bytes.
constexpr uint32_t kPayloadBitsPerSecond =
    kPacketsPerSecond * AudioPacket::kPacketBytes * 8;
constexpr const char* kSpiffsRoot = "/spiffs";
constexpr const char* kDefaultTrack = "song.u8";
constexpr TickType_t kRxPollPeriod = pdMS_TO_TICKS(20);
constexpr size_t kConsoleLineBytes = 160;

// These compile-time checks keep the audio format aligned with nRF24 hardware
// limits instead of failing later at runtime.
static_assert(AudioPacket::kPacketBytes <= 32, "nRF24 payload limit exceeded");
static_assert(kPayloadBitsPerSecond < 250000, "Audio stream exceeds the nRF24 250 kbps mode");

struct TrackInfo {
    std::string name;
    size_t bytes = 0;
};

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

bool sendU8Song(RadioManager& manager, const char* path)
{
    // Stream one audio file to the radio by:
    // 1. reading fixed-size chunks from SPIFFS
// 2. wrapping each chunk in the AudioPacket format
// 3. sending each packet through RadioManager
// 4. delaying long enough to preserve the original sample timing
    std::FILE* fp = std::fopen(path, "rb");
    if (!fp) {
        ESP_LOGE(TAG, "Could not open %s. Use the Prepare Demo Audio task and upload the filesystem image.", path);
        return false;
    }

    uint8_t audio_chunk[AudioPacket::kAudioBytesPerPacket];
    uint8_t packet[AudioPacket::kPacketBytes];
    uint16_t sequence = 0;

    while (true) {
        // Read one packet's worth of pulse-code modulation (PCM) data from
        // disk.
        const size_t bytes_read =
            std::fread(audio_chunk, 1, AudioPacket::kAudioBytesPerPacket, fp);
        if (bytes_read == 0) {
            break;
        }

        size_t packet_len = 0;
        // A short read means we reached the tail of the file and should mark
        // the packet as the final one in the stream.
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
        // Rate-limit transmission so the receiver sees roughly real-time audio
        // pacing instead of a burst of packets all at once.
        const uint32_t chunk_ms = static_cast<uint32_t>((bytes_read * 1000u) / kSampleRateHz);
        vTaskDelay(pdMS_TO_TICKS(chunk_ms > 0 ? chunk_ms : 1));

        if (is_last) {
            break;
        }
    }

    std::fclose(fp);
    ESP_LOGI(TAG, "Finished sending %u packets", sequence);
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

        if (boot_ok) {
            ESP_LOGI(TAG, "Radio boot OK, state=%s", RadioManager::stateName(status.state));
        } else {
            ESP_LOGI(TAG, "Radio unavailable, console running in filesystem-only mode.");
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
                    handleCommand(command_line);
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
            "  FILES                List available .u8 tracks in SPIFFS\n"
            "  SELECT <file>        Choose which SPIFFS track TX will send\n"
            "  TX [file]            Send the selected track, or another track if provided\n"
            "  RX                   Enter receive/listen mode\n"
            "  STANDBY              Leave RX/CW/sleep and return to standby\n"
            "  SLEEP                Put the radio into sleep mode\n"
            "  WAKE                 Wake the radio back to standby\n"
            "  POWERDOWN            Fully power down the radio\n"
            "  CHANNEL <0-125>      Reinitialize the radio on a new channel\n"
            "  CW START [ch] [0-3]  Start a continuous-wave test on a channel/power level\n"
            "  CW STOP              Stop the continuous-wave test\n"
            "\nPrepare new tracks on the host with the PlatformIO 'Prepare Demo Audio' target\n"
            "or by running: python tools/prepare_demo_audio.py <path-to-song.mp3>\n");
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

    void printStatus()
    {
        // STATUS combines the cached RadioStatus snapshot with a live
        // hasPendingRx() check when receive (RX) mode is active.
        if (!takeRadio()) {
            std::printf("Could not read radio status right now.\n");
            return;
        }

        const RadioStatus status = manager_.status();
        const bool rx_pending =
            status.state == RadioState::RxListening ? manager_.hasPendingRx() : false;
        giveRadio();

        std::printf("State=%s channel=%u selected=%s last_status=0x%02X tx_ok=%s rx_len=%u fault=%d",
                    RadioManager::stateName(status.state),
                    static_cast<unsigned>(status.channel),
                    selected_track_.c_str(),
                    static_cast<unsigned>(status.last_status),
                    status.last_tx_ok ? "true" : "false",
                    static_cast<unsigned>(status.last_rx_len),
                    status.last_fault);
        if (status.state == RadioState::RxListening) {
            std::printf(" rx_pending=%s", rx_pending ? "true" : "false");
        }
        std::printf("\n");
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

    bool commandTx(const std::vector<std::string>& words)
    {
        // Transmit (TX) either uses the explicitly requested file or the
        // currently
        // selected default track.
        const std::string request = words.size() >= 2 ? words[1] : selected_track_;
        TrackInfo track{};
        if (!resolveTrack(request, track)) {
            std::printf("Track '%s' was not found in SPIFFS.\n", request.c_str());
            return false;
        }

        if (!takeRadio()) {
            std::printf("Radio is busy.\n");
            return false;
        }

        // Keep the radio state transitions serialized with the actual file
        // transmission so background receive (RX) polling does not interfere.
        bool ok = ensureStandbyLocked();
        if (ok) {
            selected_track_ = track.name;
            const std::string path = buildTrackPath(selected_track_);
            ESP_LOGI(TAG, "Starting TX for %s", path.c_str());
            ok = sendU8Song(manager_, path.c_str());
        }

        const RadioStatus status = manager_.status();
        giveRadio();

        if (!ok) {
            std::printf("TX failed. state=%s fault=%d\n",
                        RadioManager::stateName(status.state),
                        status.last_fault);
            return false;
        }

        std::printf("TX complete for %s\n", selected_track_.c_str());
        return true;
    }

    bool commandRx()
    {
        // Receive (RX) is a persistent mode rather than a one-shot action. The
        // background
        // rxTask() is what actually polls for and logs packets afterward.
        if (!takeRadio()) {
            std::printf("Radio is busy.\n");
            return false;
        }

        bool ok = ensureStandbyLocked();
        if (ok) {
            ok = manager_.enterRx();
        }

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
            std::printf("Usage: CW START [channel] [power0-3] | CW STOP\n");
            return false;
        }

        const std::string action = uppercaseCopy(words[1]);
        if (action == "STOP") {
            if (!takeRadio()) {
                std::printf("Radio is busy.\n");
                return false;
            }

            bool ok = true;
            if (manager_.status().state == RadioState::CwTest) {
                ok = manager_.stopCw();
            }

            const RadioStatus status = manager_.status();
            giveRadio();

            if (!ok) {
                std::printf("Could not stop CW mode. state=%s fault=%d\n",
                            RadioManager::stateName(status.state),
                            status.last_fault);
                return false;
            }

            std::printf("CW mode stopped.\n");
            return true;
        }

        if (action != "START") {
            std::printf("Usage: CW START [channel] [power0-3] | CW STOP\n");
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

        std::printf("CW mode active on channel %u at power level %u\n",
                    static_cast<unsigned>(status.channel),
                    static_cast<unsigned>(power_level));
        return true;
    }

    void logRxPacket(const uint8_t* payload, size_t len)
    {
        // Receive (RX) logging tries to interpret the bytes as the project's
        // packet format first, then falls back to reporting a raw fixed-width
        // frame.
        AudioPacket::Header header{};
        const uint8_t* audio = nullptr;
        if (AudioPacket::decode(payload, len, header, audio)) {
            ESP_LOGI(TAG, "RX packet seq=%u audio=%u flags=0x%02X",
                     static_cast<unsigned>(header.sequence),
                     static_cast<unsigned>(header.audio_len),
                     static_cast<unsigned>(header.flags));
            return;
        }

        ESP_LOGI(TAG, "RX payload len=%u (fixed-width frame)", static_cast<unsigned>(len));
    }

    void rxTask()
    {
        // This background task is intentionally conservative:
        // - it tries to grab the mutex briefly
        // - it only touches the radio when receive (RX) mode is active
        // - it logs received packets but does not otherwise mutate app state
        std::array<uint8_t, AudioPacket::kPacketBytes> payload{};

        while (true) {
            if (takeRadio(pdMS_TO_TICKS(10))) {
                if (manager_.status().state == RadioState::RxListening && manager_.hasPendingRx()) {
                    size_t out_len = 0;
                    if (manager_.receivePayload(payload.data(), payload.size(), out_len)) {
                        logRxPacket(payload.data(), out_len);
                    } else {
                        const RadioStatus status = manager_.status();
                        ESP_LOGW(TAG, "RX read failed, state=%s fault=%d",
                                 RadioManager::stateName(status.state),
                                 status.last_fault);
                    }
                }
                giveRadio();
            }

            vTaskDelay(kRxPollPeriod);
        }
    }

    bool handleCommand(const std::string& line)
    {
        // Command dispatch is a simple keyword router. Each handler owns its
        // own validation and user-facing error messages.
        const std::vector<std::string> words = splitWords(line);
        if (words.empty()) {
            return true;
        }

        const std::string command = uppercaseCopy(words.front());
        if (command == "HELP" || command == "?") {
            printHelp();
            return true;
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
            return commandTx(words);
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

    RadioManager& manager_;
    SemaphoreHandle_t radio_mutex_ = nullptr;  // Serializes all radio access.
    TaskHandle_t rx_task_ = nullptr;           // Background receive (RX) polling task.
    std::string selected_track_ = kDefaultTrack;  // Default file used by transmit (TX).
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
             "nRF24 pins: SCK=%d MISO=%d MOSI=%d CE=%d CSN=%d",
             static_cast<int>(config.sck_pin),
             static_cast<int>(config.miso_pin),
             static_cast<int>(config.mosi_pin),
             static_cast<int>(config.ce_pin),
             static_cast<int>(config.csn_pin));

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
