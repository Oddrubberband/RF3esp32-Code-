#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
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
#include <utility>
#include <vector>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_random.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "audio_packet.hpp"
#include "app_status.hpp"
#include "command_parser.hpp"
#include "esp32_nrf24_hal.hpp"
#include "file_transfer_service.hpp"
#include "hardware_profile.hpp"
#include "morse.hpp"
#include "nrf24.hpp"
#include "protocol_v2.hpp"
#include "radio_channel.hpp"
#include "radio_manager.hpp"
#include "reliable_transfer_v2.hpp"
#include "rx_drain.hpp"
#include "tx_helpers.hpp"
#include "transfer_rate_limiter.hpp"
#include "validation.hpp"
#include "stream_sync.hpp"
#include "wifi_control_config.hpp"

#ifndef WIRELESS_CONTROL_ENABLED
#define WIRELESS_CONTROL_ENABLED 0
#endif

#ifndef WIRELESS_CONTROL_AUTO_RX
#define WIRELESS_CONTROL_AUTO_RX 0
#endif

#ifndef RF3_WIFI_CONTROL_ENABLED
#define RF3_WIFI_CONTROL_ENABLED 0
#endif

#ifndef RF3_TRANSFER_RATE_LIMIT_BPS
#define RF3_TRANSFER_RATE_LIMIT_BPS 0
#endif

#ifndef RF3_VERBOSE_RX_LOG
#define RF3_VERBOSE_RX_LOG 0
#endif

// main.cpp owns the top-level demo flow:
// - mount the Serial Peripheral Interface Flash File System (SPIFFS) partition
//   that stores staged data files
// - initialize the nRF24 radio
// - expose a serial-console command loop
// - run a small background task that polls receive (RX) mode
//
// Most of the project's user-visible behavior lives here, while lower-level
// files keep packet formatting and radio access focused and testable.
namespace {
using CommandParsing::parseLoopCountToken;
using CommandParsing::parseUint32Arg;
using CommandParsing::parseUint8Arg;
using CommandParsing::splitWords;
using CommandParsing::trimAscii;
using CommandParsing::uppercaseCopy;

constexpr const char* TAG = "APP";
constexpr const char* kSpiffsRoot = "/spiffs";
constexpr const char* kDefaultFile = "payload.bin";
constexpr const char* kReceivedFilePrefix = "rx_";
constexpr const char* kReceivedFileExtension = ".bin";
constexpr const char* kReceivedPartialExtension = ".part";
constexpr uint16_t kMaxReceivedFileCollisionIndex = 999;
constexpr uint32_t kDefaultMorseDotMs = 120;
constexpr uint8_t kDefaultMorsePowerLevel = 3;
constexpr TickType_t kLoopWorkerPeriod = pdMS_TO_TICKS(20);
constexpr TickType_t kLoopStopPollPeriod = pdMS_TO_TICKS(20);
// A peerless CANCEL can consume six 500 ms control-response windows (the first
// attempt plus five bounded retries). Allow that state machine to terminate.
constexpr TickType_t kLoopStopTimeout = pdMS_TO_TICKS(4000);
constexpr TickType_t kWifiControlPollPeriod = pdMS_TO_TICKS(100);
// The nRF24 RX FIFO is only three packets deep, so polling much slower than
// the packet cadence will overrun the queue during live data transfer.
static_assert(pdMS_TO_TICKS(2) > 0,
              "RF3 requires a FreeRTOS tick rate that represents its 2 ms transfer poll");
constexpr TickType_t kRxPollPeriod = pdMS_TO_TICKS(2);
constexpr TickType_t kWifiConnectTimeout = pdMS_TO_TICKS(15000);
constexpr size_t kConsoleLineBytes = 160;
constexpr bool kWirelessControlEnabled = WIRELESS_CONTROL_ENABLED != 0;
constexpr bool kWirelessControlAutoRx = WIRELESS_CONTROL_AUTO_RX != 0;
constexpr bool kWifiControlEnabled = RF3_WIFI_CONTROL_ENABLED != 0;
constexpr uint32_t kTransferRateLimitBytesPerSecond = RF3_TRANSFER_RATE_LIMIT_BPS;
constexpr EventBits_t kWifiConnectedBit = BIT0;
constexpr char kWifiPlaceholderSsid[] = "YOUR_WIFI_SSID";
constexpr char kWifiPlaceholderPassword[] = "YOUR_WIFI_PASSWORD";

// These compile-time checks keep the packetized transfer settings aligned with
// nRF24 hardware limits instead of failing later at runtime.
static_assert(ProtocolV2::kFrameSize == AudioPacket::kPacketBytes,
              "Protocol v2 and nRF24 frame widths differ");

struct FileInfo {
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
    std::string file_name;
    std::string morse_text;
    uint32_t cw_on_ms = 0;
    uint32_t cw_off_ms = 0;
    uint32_t cw_report_every = 0;
    uint8_t channel = 76;
    uint8_t power_level = 3;
};

struct IncomingFileStorage {
    uint32_t transfer_id = 0;
    std::string final_name;
    std::string partial_name;
    std::FILE* file = nullptr;
};

using ProtocolTransferReport = AppStatus::TransferReport;

struct StatusMutex {
    SemaphoreHandle_t handle = nullptr;
    void lock() { (void)xSemaphoreTake(handle, portMAX_DELAY); }
    void unlock() { xSemaphoreGive(handle); }
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

bool statFileSize(const std::string& path, size_t& bytes)
{
    // SPIFFS directory listings only provide file names. stat() is used to get
    // an accurate byte count for user-interface (UI) output and file
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

uint64_t monotonicMilliseconds()
{
    const int64_t microseconds = esp_timer_get_time();
    return microseconds > 0 ? static_cast<uint64_t>(microseconds) / 1000u : 0u;
}

void delayAtLeastMs(uint32_t duration_ms)
{
    const TickType_t ticks = pdMS_TO_TICKS(duration_ms);
    vTaskDelay(ticks > 0 ? ticks : 1);
}

bool sendPayloadWithRetry(RadioManager& manager, const uint8_t* payload, size_t len)
{
    return TxHelpers::sendWithRetry(
        [&manager, payload, len]() {
            return manager.sendPayload(payload, len);
        },
        [](uint32_t delay_ms) {
            delayAtLeastMs(delay_ms);
        });
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

// ---- SPIFFS file discovery helpers ---------------------------------------

std::string buildFilePath(std::string_view name)
{
    // Accept either a bare file name or an already-qualified /spiffs path so
    // higher-level code can stay flexible.
    if (name.rfind("/spiffs/", 0) == 0) {
        return std::string(name);
    }
    return std::string(kSpiffsRoot) + "/" + std::string(name);
}

std::string buildReceivedFileName(uint32_t transfer_id,
                                  bool partial,
                                  uint16_t collision_index = 0)
{
    char buffer[40] = {};
    if (partial || collision_index == 0) {
        std::snprintf(buffer,
                      sizeof(buffer),
                      "%s%08lX%s",
                      kReceivedFilePrefix,
                      static_cast<unsigned long>(transfer_id),
                      partial ? kReceivedPartialExtension : kReceivedFileExtension);
    } else {
        std::snprintf(buffer,
                      sizeof(buffer),
                      "%s%08lX_%03u%s",
                      kReceivedFilePrefix,
                      static_cast<unsigned long>(transfer_id),
                      static_cast<unsigned>(collision_index),
                      kReceivedFileExtension);
    }
    return std::string(buffer);
}

bool resolveFile(std::string request, FileInfo& out)
{
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
    if (ReliableTransferV2::isInternalTransferName(request)) {
        return false;
    }

    size_t bytes = 0;
    if (statFileSize(buildFilePath(request), bytes)) {
        out.name = request;
        out.bytes = bytes;
        return true;
    }

    return false;
}

std::vector<FileInfo> listAllFiles(bool* scan_ok = nullptr)
{
    std::vector<FileInfo> files;
    if (scan_ok) {
        *scan_ok = false;
    }
    DIR* dir = opendir(kSpiffsRoot);
    if (!dir) {
        return files;
    }

    while (dirent* entry = readdir(dir)) {
        const std::string name(entry->d_name);
        if (name == "." || name == "..") {
            continue;
        }

        size_t bytes = 0;
        if (!statFileSize(buildFilePath(name), bytes)) {
            continue;
        }

        files.push_back({name, bytes});
    }

    closedir(dir);
    if (scan_ok) {
        *scan_ok = true;
    }
    std::sort(files.begin(), files.end(), [](const FileInfo& lhs, const FileInfo& rhs) {
        return lhs.name < rhs.name;
    });
    return files;
}

std::vector<FileInfo> listFiles()
{
    std::vector<FileInfo> files;
    for (const FileInfo& file : listAllFiles()) {
        if (!file.name.empty() && file.name.front() != '.' &&
            !ReliableTransferV2::isInternalTransferName(file.name)) {
            files.push_back(file);
        }
    }
    return files;
}

void cleanupStaleIncomingFiles()
{
    DIR* dir = opendir(kSpiffsRoot);
    if (!dir) {
        ESP_LOGW(TAG, "Could not inspect SPIFFS for stale RX partial files: errno=%d", errno);
        return;
    }

    while (dirent* entry = readdir(dir)) {
        const std::string name(entry->d_name);
        if (!ReliableTransferV2::isInternalTransferName(name)) {
            continue;
        }

        const std::string path = buildFilePath(name);
        errno = 0;
        if (std::remove(path.c_str()) == 0 || errno == ENOENT) {
            ESP_LOGI(TAG, "Removed stale RX partial file %s", name.c_str());
        } else {
            ESP_LOGW(TAG,
                     "Could not remove stale RX partial file %s: errno=%d",
                     name.c_str(),
                     errno);
        }
    }

    closedir(dir);
}

void printFileTable(const std::vector<FileInfo>& files, std::string_view selected_file)
{
    if (files.empty()) {
        std::printf("No staged files are available in SPIFFS.\n");
        return;
    }

    std::printf("Files in SPIFFS:\n");
    for (const FileInfo& file : files) {
        const char* marker = file.name == selected_file ? "*" : " ";
        std::printf(" %s %s (%u bytes)\n",
                    marker,
                    file.name.c_str(),
                    static_cast<unsigned>(file.bytes));
    }
}

// ---- startup helpers ------------------------------------------------------

bool mountFileSystem()
{
    // The app expects a prebuilt SPIFFS image containing staged data files. It
    // does not auto-format on failure because an empty partition is more
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

bool resetFileSource(void* context)
{
    std::FILE* file = static_cast<std::FILE*>(context);
    if (!file) {
        return false;
    }
    std::clearerr(file);
    return std::fseek(file, 0, SEEK_SET) == 0;
}

ReliableTransferV2::ReadResult readFileSource(void* context,
                                              uint8_t* out,
                                              size_t capacity)
{
    std::FILE* file = static_cast<std::FILE*>(context);
    if (!file || !out || capacity == 0) {
        return {0, ReliableTransferV2::ReadState::Error};
    }
    const size_t bytes_read = std::fread(out, 1, capacity, file);
    if (std::ferror(file)) {
        return {bytes_read, ReliableTransferV2::ReadState::Error};
    }
    return {bytes_read,
            std::feof(file) ? ReliableTransferV2::ReadState::EndOfFile
                            : ReliableTransferV2::ReadState::MoreDataMayFollow};
}

uint32_t nextTransferId()
{
    static uint32_t previous = 0;
    uint32_t candidate = esp_random();
    if (candidate == 0 || candidate == previous) {
        candidate = esp_random();
    }
    if (candidate == 0 || candidate == previous) {
        candidate = previous + 1u;
        if (candidate == 0) {
            candidate = 1;
        }
    }
    previous = candidate;
    return candidate;
}

uint32_t nextTransferIdEntry(void*)
{
    return nextTransferId();
}

void updateTransferReport(ProtocolTransferReport* report,
                          FileTransfer::Service& service,
                          uint64_t now_ms,
                          uint64_t start_ms)
{
    service.refreshSenderStatus(now_ms);
    if (!report) {
        return;
    }
    const ReliableTransferV2::SenderSession& sender = service.transportSender();
    report->state = sender.state();
    report->error = sender.error();
    report->transfer_id = sender.transferId();
    report->bytes_transferred = sender.acknowledgedBytes();
    report->total_bytes = sender.totalSize();
    report->current_sequence = sender.currentSequence();
    report->total_packets = sender.totalPackets();
    report->retry_count = sender.totalRetries();
    report->crc32 = sender.crc32();
    report->elapsed_ms = now_ms >= start_ms ? now_ms - start_ms : 0;
    report->throughput_bps = report->elapsed_ms == 0
                                 ? 0
                                 : static_cast<uint32_t>(
                                       (static_cast<uint64_t>(report->bytes_transferred) * 1000u) /
                                       report->elapsed_ms);
    report->peer_complete = sender.peerComplete();
}

bool sendDataFile(RadioManager& manager,
                  FileTransfer::Service& transfer_service,
                  const char* path,
                  const std::atomic_bool* stop_requested = nullptr,
                  ProtocolTransferReport* report = nullptr,
                  void (*publish_status)(void*) = nullptr,
                  void* publish_context = nullptr)
{
    const auto publish = [&]() {
        if (publish_status) publish_status(publish_context);
    };
    if (report) {
        *report = ProtocolTransferReport{};
        report->preparing = true;
    }
    publish();
    std::FILE* file = std::fopen(path, "rb");
    if (!file) {
        if (report) {
            report->preparing = false;
            report->state = ReliableTransferV2::SenderState::Failed;
            report->error = ProtocolV2::ErrorCode::SourceRead;
        }
        publish();
        ESP_LOGE(TAG, "Could not open %s for reliable transfer: errno=%d", path, errno);
        return false;
    }

    FileTransfer::StreamingDataSource source(
        file, {&resetFileSource, &readFileSource});
    FileTransfer::TransferMetadata metadata{};
    const char* separator = std::strrchr(path, '/');
    (void)metadata.logical_filename.set(separator ? separator + 1 : path);
    const std::string_view file_name = metadata.logical_filename.c_str();
    (void)metadata.media_type.set(
        file_name.size() >= 3 && file_name.substr(file_name.size() - 3) == ".u8"
            ? "audio/x-unsigned-8bit-pcm" : "application/octet-stream");
    struct stat source_info {};
    if (::stat(path, &source_info) == 0 && source_info.st_size >= 0 &&
        static_cast<uint64_t>(source_info.st_size) <= UINT32_MAX) {
        metadata.has_expected_length = true;
        metadata.expected_length = static_cast<uint32_t>(source_info.st_size);
    }

    const uint64_t start_ms = monotonicMilliseconds();
    const FileTransfer::StartResult started =
        transfer_service.startTransfer(source, metadata, start_ms);
    if (report) report->preparing = false;
    if (!started) {
        if (report) {
            report->state = ReliableTransferV2::SenderState::Failed;
            switch (started.code) {
                case FileTransfer::StartCode::Busy:
                    report->error = ProtocolV2::ErrorCode::Busy;
                    break;
                case FileTransfer::StartCode::UnsupportedSize:
                    report->error = ProtocolV2::ErrorCode::UnsupportedSize;
                    break;
                case FileTransfer::StartCode::LengthMismatch:
                    report->error = ProtocolV2::ErrorCode::InvalidMetadata;
                    break;
                case FileTransfer::StartCode::SessionRejected:
                    report->error = transfer_service.transportSender().error();
                    break;
                default:
                    report->error = ProtocolV2::ErrorCode::SourceRead;
                    break;
            }
        }
        publish();
        std::fclose(file);
        ESP_LOGE(TAG,
                 "Protocol v2 source preparation failed for %s result=%u max_bytes=%lu",
                 path,
                 static_cast<unsigned>(started.code),
                 static_cast<unsigned long>(ProtocolV2::kMaxFileSize));
        return false;
    }

    const uint32_t transfer_id = started.transfer_id;
    ReliableTransferV2::SenderSession& sender = transfer_service.transportSender();
    const auto refresh_report = [&]() {
        updateTransferReport(report, transfer_service, monotonicMilliseconds(), start_ms);
        publish();
    };
    refresh_report();

    ESP_LOGI(TAG,
             "Protocol v2 TX start id=%08lX bytes=%lu packets=%lu crc32=%08lX",
             static_cast<unsigned long>(transfer_id),
             static_cast<unsigned long>(sender.totalSize()),
             static_cast<unsigned long>(sender.totalPackets()),
             static_cast<unsigned long>(sender.crc32()));

    bool cancellation_started = false;
    uint8_t last_tx_progress_percent = 0;
    while (!sender.terminal()) {
        if (stop_requested && stop_requested->load() && !cancellation_started) {
            cancellation_started = true;
            (void)transfer_service.cancelTransfer(transfer_id, monotonicMilliseconds());
        }

        ProtocolV2::Frame outbound{};
        if (!sender.outboundFrame(outbound)) {
            (void)sender.tick(monotonicMilliseconds());
            refresh_report();
            delayAtLeastMs(2);
            continue;
        }

        const ProtocolV2::Packet outbound_packet = [&]() {
            ProtocolV2::Packet packet{};
            (void)ProtocolV2::decode(outbound.data(), outbound.size(), packet);
            return packet;
        }();

        if (outbound_packet.type == ProtocolV2::PacketType::Data) {
            const uint32_t rate_delay_ms = TransferRateLimiter::delayMilliseconds(
                outbound_packet.payload_length, kTransferRateLimitBytesPerSecond);
            if (rate_delay_ms > 0) {
                delayAtLeastMs(rate_delay_ms);
            }
        }
        const uint64_t send_time_ms = monotonicMilliseconds();
        if (!manager.sendPayload(outbound.data(), outbound.size())) {
            (void)sender.onTransportFailure(send_time_ms);
            refresh_report();
            delayAtLeastMs(2);
            continue;
        }
        if (!sender.noteFrameSent(send_time_ms)) {
            break;
        }

        if (outbound_packet.type == ProtocolV2::PacketType::End) {
            ESP_LOGI(TAG,
                     "Protocol v2 local DATA acknowledged id=%08lX; awaiting remote verification",
                     static_cast<unsigned long>(transfer_id));
        }

        if (!manager.enterRx()) {
            (void)sender.onTransportFailure(monotonicMilliseconds());
            refresh_report();
            continue;
        }

        bool leave_wait = false;
        while (!leave_wait && !sender.terminal()) {
            const uint64_t now_ms = monotonicMilliseconds();
            if (stop_requested && stop_requested->load() && !cancellation_started) {
                cancellation_started = true;
                (void)transfer_service.cancelTransfer(transfer_id, now_ms);
                leave_wait = true;
                break;
            }

            if (manager.hasPendingRx()) {
                ProtocolV2::Frame incoming{};
                size_t incoming_length = 0;
                if (!manager.receivePayload(
                        incoming.data(), incoming.size(), incoming_length)) {
                    (void)sender.onTransportFailure(now_ms);
                    leave_wait = true;
                    break;
                }
                const ReliableTransferV2::SenderEvent event = sender.onFrame(
                    incoming.data(), incoming_length, now_ms);
                leave_wait = event == ReliableTransferV2::SenderEvent::OutboundReady ||
                             event == ReliableTransferV2::SenderEvent::Completed ||
                             event == ReliableTransferV2::SenderEvent::Cancelled ||
                             event == ReliableTransferV2::SenderEvent::Failed;
            }

            if (!leave_wait) {
                const ReliableTransferV2::SenderEvent event = sender.tick(now_ms);
                leave_wait = event == ReliableTransferV2::SenderEvent::OutboundReady ||
                             event == ReliableTransferV2::SenderEvent::Completed ||
                             event == ReliableTransferV2::SenderEvent::Cancelled ||
                             event == ReliableTransferV2::SenderEvent::Failed;
            }
            if (!leave_wait) {
                refresh_report();
                delayAtLeastMs(2);
            }
        }

        if (manager.status().state == RadioState::RxListening && !manager.leaveRx()) {
            (void)sender.onTransportFailure(monotonicMilliseconds());
        }
        refresh_report();

        const uint8_t progress_percent = FileTransfer::progressMilestonePercent(
            sender.acknowledgedBytes(), sender.totalSize());
        if (progress_percent > last_tx_progress_percent) {
            last_tx_progress_percent = progress_percent;
            const uint64_t progress_now_ms = monotonicMilliseconds();
            const uint64_t progress_elapsed_ms =
                progress_now_ms >= start_ms ? progress_now_ms - start_ms : 0;
            const uint32_t progress_rate_bps = progress_elapsed_ms == 0
                ? 0
                : static_cast<uint32_t>(
                      (static_cast<uint64_t>(sender.acknowledgedBytes()) * 1000u) /
                      progress_elapsed_ms);
            ESP_LOGI(TAG,
                     "TX progress id=%08lX %u%% | bytes=%lu/%lu | packets=%lu/%lu | retries=%lu | rate=%lu B/s",
                     static_cast<unsigned long>(transfer_id),
                     static_cast<unsigned>(progress_percent),
                     static_cast<unsigned long>(sender.acknowledgedBytes()),
                     static_cast<unsigned long>(sender.totalSize()),
                     static_cast<unsigned long>(sender.acknowledgedPackets()),
                     static_cast<unsigned long>(sender.totalPackets()),
                     static_cast<unsigned long>(sender.totalRetries()),
                     static_cast<unsigned long>(progress_rate_bps));
        }
    }

    const uint64_t finish_ms = monotonicMilliseconds();
    updateTransferReport(report, transfer_service, finish_ms, start_ms);
    publish();
    const uint64_t elapsed_ms = finish_ms >= start_ms ? finish_ms - start_ms : 0;
    const uint32_t throughput_bps = elapsed_ms == 0
                                        ? 0
                                        : static_cast<uint32_t>(
                                              (static_cast<uint64_t>(sender.acknowledgedBytes()) * 1000u) /
                                              elapsed_ms);
    std::fclose(file);
    if (sender.state() == ReliableTransferV2::SenderState::Completed &&
        sender.peerComplete()) {
        ESP_LOGI(TAG,
                 "Protocol v2 remote receiver verified and published id=%08lX bytes=%lu packets=%lu crc32=%08lX retries=%lu elapsed_ms=%llu throughput_bps=%lu",
                 static_cast<unsigned long>(sender.transferId()),
                 static_cast<unsigned long>(sender.acknowledgedBytes()),
                 static_cast<unsigned long>(sender.acknowledgedPackets()),
                 static_cast<unsigned long>(sender.crc32()),
                 static_cast<unsigned long>(sender.totalRetries()),
                 static_cast<unsigned long long>(elapsed_ms),
                 static_cast<unsigned long>(throughput_bps));
        return true;
    }

    ESP_LOGE(TAG,
             "Protocol v2 transfer did not complete id=%08lX state=%s error=%s bytes=%lu/%lu seq=%lu retries=%lu elapsed_ms=%llu throughput_bps=%lu",
             static_cast<unsigned long>(sender.transferId()),
             ReliableTransferV2::senderStateName(sender.state()),
             ProtocolV2::errorName(sender.error()),
             static_cast<unsigned long>(sender.acknowledgedBytes()),
             static_cast<unsigned long>(sender.totalSize()),
             static_cast<unsigned long>(sender.currentSequence()),
             static_cast<unsigned long>(sender.totalRetries()),
             static_cast<unsigned long long>(elapsed_ms),
             static_cast<unsigned long>(throughput_bps));
    return false;
}

class DemoConsoleApp {
public:
    explicit DemoConsoleApp(RadioManager& manager)
        : manager_(manager),
          transfer_service_(&nextTransferIdEntry),
          receiver_(this, incomingStorageCallbacks())
    {
        transfer_service_.registerReceiveHandler(
            &DemoConsoleApp::receiveCompletionEntry, this);
    }

    bool initialize()
    {
        // Initialization wires together every subsystem needed before the
        // interactive console can start:
        // - create a mutex that serializes all radio access
        // - disable stdio buffering so the serial console feels live
        // - mount SPIFFS and verify the selected file exists
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

        status_mutex_.handle = xSemaphoreCreateMutex();
        if (!status_mutex_.handle) {
            ESP_LOGE(TAG, "Failed to allocate status mutex");
            return false;
        }
        status_cache_.selectFile(kDefaultFile, 0);

        std::setvbuf(stdin, nullptr, _IONBF, 0);
        std::setvbuf(stdout, nullptr, _IONBF, 0);

        filesystem_ready_ = mountFileSystem();
        if (!filesystem_ready_) {
            ESP_LOGE(TAG,
                     "SPIFFS unavailable. Upload a filesystem image; the serial console will remain available for diagnostics.");
        } else {
            cleanupStaleIncomingFiles();
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

        // If the configured default file is missing, fall back to the first
        // available SPIFFS file so the transmit (TX) command still has a sane
        // default.
        const std::vector<FileInfo> files = listFiles();
        if (!files.empty()) {
            FileInfo selected{};
            if (!resolveFile(selectedFileName(), selected)) {
                selected = files.front();
            }
            selectFile(selected);
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

        if (kWifiControlEnabled) {
            if (!startWifiControlPlane()) {
                ESP_LOGW(TAG, "Wi-Fi control plane not available. Serial console remains active.");
            }
        } else {
            ESP_LOGI(TAG, "Wi-Fi control plane disabled for this build.");
        }

        ESP_LOGI(TAG,
                 "Reliable file protocol v%u: frame=%u data=%u max_file=%lu retries=control:%u,data:%u",
                 static_cast<unsigned>(ProtocolV2::kVersion),
                 static_cast<unsigned>(ProtocolV2::kFrameSize),
                 static_cast<unsigned>(ProtocolV2::kDataPayloadCapacity),
                 static_cast<unsigned long>(ProtocolV2::kMaxFileSize),
                 static_cast<unsigned>(ProtocolV2::kMaximumControlRetries),
                 static_cast<unsigned>(ProtocolV2::kMaximumDataRetries));
        ESP_LOGI(TAG,
                 "Build profile=%s filesystem=%s HTTP-control=%s transfer-rate-limit=%lu B/s",
                 HardwareProfile::kSelectedName,
                 filesystem_ready_ ? "ready" : "unavailable",
                 kWifiControlEnabled ? "enabled" : "disabled",
                 static_cast<unsigned long>(kTransferRateLimitBytesPerSecond));
        printHelp();
        printFileTable(files, selectedFileName());
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
        bool line_too_long = false;
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
                if (line_too_long) {
                    std::printf("Command rejected: maximum length is %u characters.\n",
                                static_cast<unsigned>(kConsoleLineBytes - 1));
                    line_too_long = false;
                } else if (!command_line.empty()) {
                    dispatchCommand(command_line, CommandOrigin::Local);
                }
                prompt_visible = false;
                continue;
            }

            if (ch == '\b' || static_cast<unsigned char>(ch) == 0x7F) {
                if (!line_too_long && !pending_line.empty()) {
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
            } else {
                // Never execute a silently truncated command. Once the input
                // exceeds the fixed console bound, discard it through Enter
                // and report the error as one rejected line.
                line_too_long = true;
            }
        }
    }

private:
    static void receiveCompletionEntry(
        void* context,
        const FileTransfer::ReceiveCompletion& completion)
    {
        static_cast<DemoConsoleApp*>(context)->onReceiveCompletion(completion);
    }

    void onReceiveCompletion(const FileTransfer::ReceiveCompletion& completion)
    {
        ESP_LOGI(TAG,
                 "Subsystem RX completion callback task=%s id=%08lX bytes=%lu path=%s",
                 pcTaskGetName(nullptr),
                 static_cast<unsigned long>(completion.status.transfer_id),
                 static_cast<unsigned long>(completion.status.bytes_transferred),
                 completion.status.final_published_path.c_str());
    }

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

    static esp_err_t httpPowerHandlerEntry(httpd_req_t* req)
    {
        return static_cast<DemoConsoleApp*>(req->user_ctx)->handleHttpPower(req);
    }

    static esp_err_t httpCommandHandlerEntry(httpd_req_t* req)
    {
        return static_cast<DemoConsoleApp*>(req->user_ctx)->handleHttpCommand(req);
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

    bool tryReadHttpCommandText(httpd_req_t* req, std::string& out)
    {
        out.clear();

        const size_t query_len = httpd_req_get_url_query_len(req);
        if (query_len > 0) {
            std::string query(query_len + 1, '\0');
            if (httpd_req_get_url_query_str(req, query.data(), query.size()) != ESP_OK) {
                return false;
            }

            std::array<char, kConsoleLineBytes> value_buf{};
            if (httpd_query_key_value(query.c_str(), "value", value_buf.data(), value_buf.size()) == ESP_OK ||
                httpd_query_key_value(query.c_str(), "command", value_buf.data(), value_buf.size()) == ESP_OK) {
                out = trimAscii(value_buf.data());
                return !out.empty();
            }
        }

        if (req->content_len <= 0 || req->content_len >= static_cast<int>(kConsoleLineBytes)) {
            return false;
        }

        std::string body(static_cast<size_t>(req->content_len), '\0');
        int offset = 0;
        while (offset < req->content_len) {
            const int received =
                httpd_req_recv(req, body.data() + offset, req->content_len - offset);
            if (received <= 0) {
                return false;
            }
            offset += received;
        }

        out = trimAscii(body);
        return !out.empty();
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

        httpd_uri_t channel_preview_uri = channel_uri;
        channel_preview_uri.method = HTTP_GET;

        httpd_uri_t power_uri{};
        power_uri.uri = "/power";
        power_uri.method = HTTP_POST;
        power_uri.handler = &DemoConsoleApp::httpPowerHandlerEntry;
        power_uri.user_ctx = this;

        httpd_uri_t command_uri{};
        command_uri.uri = "/command";
        command_uri.method = HTTP_POST;
        command_uri.handler = &DemoConsoleApp::httpCommandHandlerEntry;
        command_uri.user_ctx = this;

        err = httpd_register_uri_handler(http_server_, &status_uri);
        if (err == ESP_OK) err = httpd_register_uri_handler(http_server_, &rx_uri);
        if (err == ESP_OK) err = httpd_register_uri_handler(http_server_, &tx_uri);
        if (err == ESP_OK) err = httpd_register_uri_handler(http_server_, &stop_uri);
        if (err == ESP_OK) err = httpd_register_uri_handler(http_server_, &channel_uri);
        if (err == ESP_OK) err = httpd_register_uri_handler(http_server_, &channel_preview_uri);
        if (err == ESP_OK) err = httpd_register_uri_handler(http_server_, &power_uri);
        if (err == ESP_OK) err = httpd_register_uri_handler(http_server_, &command_uri);

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
                     "Wi-Fi credentials are not configured. Use ignored include/wifi_control_config.local.hpp or RF3_WIFI_SSID/RF3_WIFI_PASSWORD build macros.");
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
        const EventBits_t bits = xEventGroupWaitBits(wifi_event_group_,
                                                     kWifiConnectedBit,
                                                     pdFALSE,
                                                     pdFALSE,
                                                     portMAX_DELAY);

        if ((bits & kWifiConnectedBit) != 0 && !http_server_) {
            (void)startHttpServer();
        }

        wifi_control_task_ = nullptr;
        vTaskDelete(nullptr);
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
        if (!parseUint8Arg(value_buf, 0, RadioChannel::kMaximum, channel)) {
            return sendHttpJson(req, "{\"error\":\"invalid_channel\"}", "400 Bad Request");
        }

        if (req->method == HTTP_GET) {
            // Preview bypasses command dispatch and status reads entirely. It
            // must not stop a loop, reset a receiver, or touch any radio register.
            std::string json;
            appendFormat(json, "{\"preview\":true,\"channel\":%u,\"frequency_mhz\":%u}",
                         static_cast<unsigned>(channel),
                         static_cast<unsigned>(RadioChannel::frequencyMHz(channel)));
            return sendHttpJson(req, json);
        }

        std::string command = "CHANNEL ";
        command += value_buf;
        const bool ok = dispatchHttpCommand(command);
        return sendHttpStatusResponse(req, ok ? "200 OK" : "500 Internal Server Error");
    }

    esp_err_t handleHttpPower(httpd_req_t* req)
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

        uint8_t power_level = 0;
        if (!parseUint8Arg(value_buf, 0, 3, power_level)) {
            return sendHttpJson(req, "{\"error\":\"invalid_power\"}", "400 Bad Request");
        }

        std::string command = "POWER ";
        command += value_buf;
        const bool ok = dispatchHttpCommand(command);
        return sendHttpStatusResponse(req, ok ? "200 OK" : "500 Internal Server Error");
    }

    esp_err_t handleHttpCommand(httpd_req_t* req)
    {
        std::string command;
        if (!tryReadHttpCommandText(req, command)) {
            return sendHttpJson(req, "{\"error\":\"missing_command\"}", "400 Bad Request");
        }

        const bool ok = dispatchHttpCommand(command);
        if (ok) {
            return sendHttpJson(req,
                                "{\"accepted\":true,\"command\":\"queued\"}",
                                "200 OK");
        }

        return sendHttpJson(req,
                            "{\"accepted\":false,\"command\":\"failed\"}",
                            "500 Internal Server Error");
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
            "  FILES                List staged files in SPIFFS\n"
            "  FS INFO              Reconcile SPIFFS usage with every file\n"
            "  FS LIST ALL          List visible, hidden, and internal files\n"
            "  FS DELETE <file>     Delete one visible file from SPIFFS\n"
            "  FS CLEAN PARTIALS    Delete stale internal receive fragments\n"
            "  FS FORMAT CONFIRM    Erase every file in SPIFFS\n"
            "  SELECT <file>        Choose which staged file TX will send\n"
            "  TX [file]            Start sending the selected or named file\n"
            "  TX LOOP [n|INF] [f]  Repeatedly send the selected or named file\n"
            "  MORSE <text>         Key A-Z/0-9/spaces as Morse; use STOP to abort\n"
            "  RX                   Enter receive/listen mode\n"
            "  STANDBY              Leave RX/CW/sleep and return to standby\n"
            "  SLEEP                Put the radio into sleep mode\n"
            "  WAKE                 Wake the radio back to standby\n"
            "  POWER <0-3>          Set packet TX power level\n"
            "  POWERDOWN            Fully power down the radio\n"
            "  CHANNEL <0-125>      Reinitialize the radio on a new channel\n"
            "  CHANNEL PREVIEW <0-125>  Show nominal MHz without changing the radio\n"
            "  CW START [ch] [0-3]  Start a continuous-wave test on a channel/power level\n"
            "  CW LOOP <on> <off>   Repeat CW bursts; optional [ch] [pwr] [EVERY <loops>]\n");

        std::printf(
            "\nStage a host file with the PlatformIO 'Stage Demo File' target\n"
            "or by running: python tools/stage_demo_file.py <path-to-file>\n");
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
        publishStatusLocked();
        xSemaphoreGive(radio_mutex_);
    }

    // Called only by the radio owner. No SPI, file I/O or command/loop locking
    // occurs while publishing, so status readers never wait for a transfer.
    void publishStatusLocked()
    {
        AppStatus::RadioSnapshot snapshot{};
        snapshot.radio = manager_.status();
        snapshot.tx = last_tx_report_;
        snapshot.updated_ms = monotonicMilliseconds();
        snapshot.rx_pending = snapshot.radio.state == RadioState::RxListening &&
                              (snapshot.radio.last_fifo_status & 0x01u) == 0;
        snapshot.carrier_events = carrier_event_count_;
        snapshot.rx_stream = decoded_rx_packet_count_;
        snapshot.rx_raw = raw_rx_packet_count_;
        snapshot.rx_missing = missing_rx_packet_count_;
        snapshot.rx_duplicates = duplicate_rx_packet_count_;
        snapshot.rx_drain_hits = rx_drain_limit_hit_count_;
        snapshot.rx_saved = saved_rx_file_count_;
        snapshot.rx_saved_bytes = saved_rx_byte_count_;
        snapshot.receiver_state = receiver_.state();
        snapshot.receiver_error = receiver_.error();
        snapshot.receiver_id = receiver_.transferId();
        snapshot.receiver_bytes = receiver_.acceptedBytes();
        snapshot.receiver_total_bytes = receiver_.totalSize();
        snapshot.receiver_sequence = receiver_.expectedSequence();
        snapshot.receiver_packets = receiver_.totalPackets();
        snapshot.receiver_crc = receiver_.calculatedCrc32();
        status_cache_.publish(snapshot);
    }

    static void publishTransferStatusEntry(void* context)
    {
        static_cast<DemoConsoleApp*>(context)->publishStatusLocked();
    }

    std::string selectedFileName() const
    {
        return status_cache_.capture().selected_name;
    }

    void selectFile(const FileInfo& file)
    {
        status_cache_.selectFile(file.name, static_cast<uint32_t>(file.bytes));
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

    static ReliableTransferV2::SinkPrepareResult prepareIncomingStorageEntry(
        void* context,
        uint32_t transfer_id,
        uint32_t total_size)
    {
        return static_cast<DemoConsoleApp*>(context)->prepareIncomingStorage(
            transfer_id, total_size);
    }

    static size_t writeIncomingStorageEntry(void* context,
                                            const uint8_t* data,
                                            size_t length)
    {
        return static_cast<DemoConsoleApp*>(context)->writeIncomingStorage(data, length);
    }

    static bool closeIncomingStorageEntry(void* context)
    {
        return static_cast<DemoConsoleApp*>(context)->closeIncomingStorage();
    }

    static bool publishIncomingStorageEntry(void* context)
    {
        return static_cast<DemoConsoleApp*>(context)->publishIncomingStorage();
    }

    static bool removeIncomingPartialEntry(void* context)
    {
        return static_cast<DemoConsoleApp*>(context)->removeIncomingPartial();
    }

    static ReliableTransferV2::SinkCallbacks incomingStorageCallbacks()
    {
        return {
            &DemoConsoleApp::prepareIncomingStorageEntry,
            &DemoConsoleApp::writeIncomingStorageEntry,
            &DemoConsoleApp::closeIncomingStorageEntry,
            &DemoConsoleApp::publishIncomingStorageEntry,
            &DemoConsoleApp::removeIncomingPartialEntry,
        };
    }

    bool selectAvailableFinalName(uint32_t transfer_id, std::string& out_name)
    {
        for (uint16_t collision = 0;
             collision <= kMaxReceivedFileCollisionIndex;
             ++collision) {
            const std::string candidate = buildReceivedFileName(transfer_id, false, collision);
            const std::string path = buildFilePath(candidate);
            struct stat info {};
            errno = 0;
            if (::stat(path.c_str(), &info) == 0) {
                continue;
            }
            if (errno == ENOENT) {
                out_name = candidate;
                return true;
            }

            ESP_LOGE(TAG,
                     "Could not inspect RX final path %s: errno=%d",
                     path.c_str(),
                     errno);
            return false;
        }

        ESP_LOGE(TAG,
                 "No collision-free RX final name is available for transfer %08lX",
                 static_cast<unsigned long>(transfer_id));
        return false;
    }

    ReliableTransferV2::SinkPrepareResult prepareIncomingStorage(
        uint32_t transfer_id,
        uint32_t total_size)
    {
        incoming_file_ = IncomingFileStorage{};
        incoming_file_.transfer_id = transfer_id;
        incoming_file_.partial_name = buildReceivedFileName(transfer_id, true);
        if (!selectAvailableFinalName(transfer_id, incoming_file_.final_name)) {
            return ReliableTransferV2::SinkPrepareResult::OpenFailed;
        }

        const std::string partial_path = buildFilePath(incoming_file_.partial_name);
        struct stat info {};
        errno = 0;
        if (::stat(partial_path.c_str(), &info) == 0) {
            if (std::remove(partial_path.c_str()) != 0) {
                ESP_LOGE(TAG,
                         "Could not remove stale RX partial %s: errno=%d",
                         partial_path.c_str(),
                         errno);
                return ReliableTransferV2::SinkPrepareResult::CleanupFailed;
            }
        } else if (errno != ENOENT) {
            ESP_LOGE(TAG,
                     "Could not inspect RX partial %s: errno=%d",
                     partial_path.c_str(),
                     errno);
            return ReliableTransferV2::SinkPrepareResult::CleanupFailed;
        }

        size_t filesystem_total = 0;
        size_t filesystem_used = 0;
        const esp_err_t info_result =
            esp_spiffs_info(nullptr, &filesystem_total, &filesystem_used);
        if (info_result != ESP_OK) {
            ESP_LOGE(TAG,
                     "Could not inspect RX storage for transfer %08lX: %s",
                     static_cast<unsigned long>(transfer_id),
                     esp_err_to_name(info_result));
            return ReliableTransferV2::SinkPrepareResult::OpenFailed;
        }
        const uint64_t safe_available = FileTransfer::safeReceiveCapacity(
            filesystem_total, filesystem_used);
        if (static_cast<uint64_t>(total_size) > safe_available) {
            ESP_LOGE(TAG,
                     "RX storage rejected id=%08lX: need=%lu safe_available=%llu used=%u total=%u reserve=25%%",
                     static_cast<unsigned long>(transfer_id),
                     static_cast<unsigned long>(total_size),
                     static_cast<unsigned long long>(safe_available),
                     static_cast<unsigned>(filesystem_used),
                     static_cast<unsigned>(filesystem_total));
            return ReliableTransferV2::SinkPrepareResult::InsufficientStorage;
        }

        incoming_file_.file = std::fopen(partial_path.c_str(), "wb");
        if (!incoming_file_.file) {
            ESP_LOGE(TAG,
                     "Could not open %s for RX transfer %08lX (%lu bytes): errno=%d",
                     partial_path.c_str(),
                     static_cast<unsigned long>(transfer_id),
                     static_cast<unsigned long>(total_size),
                     errno);
            return ReliableTransferV2::SinkPrepareResult::OpenFailed;
        }
        return ReliableTransferV2::SinkPrepareResult::Ready;
    }

    size_t writeIncomingStorage(const uint8_t* data, size_t length)
    {
        if (!incoming_file_.file || !data || length == 0) {
            return 0;
        }
        errno = 0;
        const size_t written = std::fwrite(data, 1, length, incoming_file_.file);
        if (written != length) {
            ESP_LOGE(TAG,
                     "RX storage write failed id=%08lX file=%s requested=%u written=%u accepted=%lu errno=%d",
                     static_cast<unsigned long>(incoming_file_.transfer_id),
                     incoming_file_.partial_name.c_str(),
                     static_cast<unsigned>(length),
                     static_cast<unsigned>(written),
                     static_cast<unsigned long>(receiver_.acceptedBytes()),
                     errno);
        }
        return written;
    }

    bool closeIncomingStorage()
    {
        if (!incoming_file_.file) {
            return true;
        }

        std::FILE* file = incoming_file_.file;
        incoming_file_.file = nullptr;
        bool ok = true;
        if (std::fflush(file) != 0) {
            ESP_LOGE(TAG, "Could not flush RX partial file: errno=%d", errno);
            ok = false;
        }
        if (std::fclose(file) != 0) {
            ESP_LOGE(TAG, "Could not close RX partial file: errno=%d", errno);
            ok = false;
        }
        return ok;
    }

    bool publishIncomingStorage()
    {
        if (incoming_file_.file || incoming_file_.partial_name.empty()) {
            return false;
        }

        if (!selectAvailableFinalName(incoming_file_.transfer_id,
                                      incoming_file_.final_name)) {
            return false;
        }

        const std::string partial_path = buildFilePath(incoming_file_.partial_name);
        const std::string final_path = buildFilePath(incoming_file_.final_name);
        if (std::rename(partial_path.c_str(), final_path.c_str()) != 0) {
            ESP_LOGE(TAG,
                     "Could not publish RX file %s -> %s for transfer %08lX: errno=%d",
                     partial_path.c_str(),
                     final_path.c_str(),
                     static_cast<unsigned long>(incoming_file_.transfer_id),
                     errno);
            return false;
        }

        incoming_file_.partial_name.clear();
        return true;
    }

    bool removeIncomingPartial()
    {
        bool ok = closeIncomingStorage();
        if (incoming_file_.partial_name.empty()) {
            return ok;
        }

        const std::string partial_path = buildFilePath(incoming_file_.partial_name);
        errno = 0;
        if (std::remove(partial_path.c_str()) != 0 && errno != ENOENT) {
            ESP_LOGE(TAG,
                     "Could not remove RX partial %s: errno=%d",
                     partial_path.c_str(),
                     errno);
            return false;
        }
        incoming_file_.partial_name.clear();
        return ok;
    }

    void discardIncomingFileTransfer(
        const char* reason,
        ProtocolV2::ErrorCode failure = ProtocolV2::ErrorCode::Cancelled)
    {
        if ((receiver_.active() ||
             receiver_.state() == ReliableTransferV2::ReceiverState::Failed) &&
            reason && *reason) {
            ESP_LOGW(TAG,
                     "%s transfer=%08lX file=%s bytes=%lu packets=%lu",
                     reason,
                     static_cast<unsigned long>(receiver_.transferId()),
                     incoming_file_.partial_name.c_str(),
                     static_cast<unsigned long>(receiver_.acceptedBytes()),
                     static_cast<unsigned long>(receiver_.acceptedPackets()));
        }

        const bool had_incomplete_transfer =
            receiver_.active() ||
            receiver_.state() == ReliableTransferV2::ReceiverState::Failed;
        if (had_incomplete_transfer) {
            (void)receiver_.abort(failure);
            transfer_service_.reportReceiveFailed(
                receiver_.transferId(), receiver_.error(), monotonicMilliseconds());
        } else {
            (void)receiver_.reset();
        }
        active_api_receive_id_ = 0;
        if (receiver_.cleanupFailed()) {
            ESP_LOGE(TAG,
                     "RX partial cleanup failed transfer=%08lX file=%s",
                     static_cast<unsigned long>(receiver_.transferId()),
                     incoming_file_.partial_name.c_str());
        }
    }

    void resetRxSession()
    {
        if (!receiver_.reset()) {
            ESP_LOGE(TAG, "RX session reset could not remove its partial file");
        }
        active_api_receive_id_ = 0;
        last_carrier_detected_ = false;
        carrier_event_count_ = 0;
        decoded_rx_packet_count_ = 0;
        raw_rx_packet_count_ = 0;
        missing_rx_packet_count_ = 0;
        duplicate_rx_packet_count_ = 0;
        rx_drain_limit_hit_count_ = 0;
        saved_rx_file_count_ = 0;
        saved_rx_byte_count_ = 0;
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
            loop_stop_requested_.store(false);
            return true;
        }

        loop_stop_requested_.store(true);
        TickType_t waited = 0;
        while (waited < timeout) {
            if (!isLoopActive()) {
                loop_stop_requested_.store(false);
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
            if (loop_stop_requested_.load()) {
                return false;
            }

            const uint32_t slice_ms = std::min<uint32_t>(remaining_ms, 20);
            delayAtLeastMs(slice_ms);
            remaining_ms -= slice_ms;
        }

        return !loop_stop_requested_.load();
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
            discardIncomingFileTransfer("Discarding partial RX file while leaving RX");
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
        // REMOTE cannot recursively relay. Filesystem mutation stays local so
        // an RF peer cannot erase or format another board's storage.
        return command != "REMOTE" && command != "FS";
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
        const AppStatus::Snapshot snapshot = status_cache_.capture();
        const AppStatus::RadioSnapshot& cached = snapshot.status;
        const RadioStatus& status = cached.radio;
        const bool rx_pending = cached.rx_pending;
        const uint32_t carrier_events = cached.carrier_events;
        const uint32_t decoded_packets = cached.rx_stream;
        const uint32_t raw_packets = cached.rx_raw;
        const uint32_t missing_packets = cached.rx_missing;
        const uint32_t duplicate_packets = cached.rx_duplicates;
        const uint32_t drain_limit_hits = cached.rx_drain_hits;
        const ProtocolTransferReport& tx_report = cached.tx;
        const ReliableTransferV2::ReceiverState receiver_state = cached.receiver_state;
        const ProtocolV2::ErrorCode receiver_error = cached.receiver_error;
        const uint32_t receiver_id = cached.receiver_id;
        const uint32_t receiver_bytes = cached.receiver_bytes;
        const uint32_t receiver_total_bytes = cached.receiver_total_bytes;
        const uint32_t receiver_sequence = cached.receiver_sequence;
        const uint32_t receiver_packets = cached.receiver_packets;
        const uint32_t receiver_crc = cached.receiver_crc;
        const LoopConfig loop = loopSnapshot();
        const char* irq_state =
            !status.irq_connected ? "disabled" : (status.irq_asserted ? "low" : "high");

        std::printf("\n=== RF3 STATUS =====================================================\n");
        std::printf("Cache  : updated=%llu ms  tx_preparing=%s\n",
                    static_cast<unsigned long long>(cached.updated_ms),
                    tx_report.preparing ? "yes" : "no");
        std::printf("System : profile=%s  filesystem=%s  http=%s  protocol=v%u\n",
                    HardwareProfile::kSelectedName,
                    filesystem_ready_ ? "ready" : "unavailable",
                    kWifiControlEnabled ? "enabled" : "disabled",
                    static_cast<unsigned>(ProtocolV2::kVersion));
        std::printf("Radio  : state=%s  channel=%u (%u MHz)  power=",
                    RadioManager::stateName(status.state),
                    static_cast<unsigned>(status.channel),
                    static_cast<unsigned>(RadioChannel::frequencyMHz(status.channel)));
        if (status.power_level >= 0) {
            std::printf("%d", status.power_level);
        } else {
            std::printf("unknown");
        }
        std::printf("  fault=%d\n", status.last_fault);
        std::printf("Regs   : STATUS=0x%02X  FIFO=0x%02X  OBSERVE_TX=0x%02X  IRQ=%s\n",
                    static_cast<unsigned>(status.last_status),
                    static_cast<unsigned>(status.last_fifo_status),
                    static_cast<unsigned>(status.last_observe_tx),
                    irq_state);
        std::printf("Live   : pending=%s  rpd=%s  last_rx_len=%u  radio_rx_packets=%u\n",
                    status.state == RadioState::RxListening
                        ? (rx_pending ? "yes" : "no") : "n/a",
                    status.state == RadioState::RxListening
                        ? (status.carrier_detected ? "high" : "low") : "n/a",
                    static_cast<unsigned>(status.last_rx_len),
                    static_cast<unsigned>(status.rx_packets));
        std::printf("File   : selected=%s  saved=%u file(s) / %u bytes\n",
                    snapshot.selected_name.c_str(),
                    static_cast<unsigned>(cached.rx_saved),
                    static_cast<unsigned>(cached.rx_saved_bytes));
        std::printf("TX     : state=%s  id=%08lX  error=%s  peer_complete=%s\n",
                    ReliableTransferV2::senderStateName(tx_report.state),
                    static_cast<unsigned long>(tx_report.transfer_id),
                    ProtocolV2::errorName(tx_report.error),
                    tx_report.peer_complete ? "yes" : "no");
        std::printf("         progress=%lu/%lu bytes  packets=%lu/%lu  retries=%lu\n",
                    static_cast<unsigned long>(tx_report.bytes_transferred),
                    static_cast<unsigned long>(tx_report.total_bytes),
                    static_cast<unsigned long>(tx_report.current_sequence),
                    static_cast<unsigned long>(tx_report.total_packets),
                    static_cast<unsigned long>(tx_report.retry_count));
        std::printf("         crc32=%08lX  elapsed=%llu ms  rate=%lu B/s\n",
                    static_cast<unsigned long>(tx_report.crc32),
                    static_cast<unsigned long long>(tx_report.elapsed_ms),
                    static_cast<unsigned long>(tx_report.throughput_bps));
        std::printf("         last_radio: irq_seen=%s  ok=%s  timeout=%s\n",
                    status.last_tx_saw_irq ? "true" : "false",
                    status.last_tx_ok ? "true" : "false",
                    status.last_tx_timed_out ? "true" : "false");
        std::printf("RX     : state=%s  id=%08lX  error=%s\n",
                    ReliableTransferV2::receiverStateName(receiver_state),
                    static_cast<unsigned long>(receiver_id),
                    ProtocolV2::errorName(receiver_error));
        std::printf("         progress=%lu/%lu bytes  packets=%lu/%lu  crc32=%08lX\n",
                    static_cast<unsigned long>(receiver_bytes),
                    static_cast<unsigned long>(receiver_total_bytes),
                    static_cast<unsigned long>(receiver_sequence),
                    static_cast<unsigned long>(receiver_packets),
                    static_cast<unsigned long>(receiver_crc));
        std::printf("         decoded=%u  raw=%u  missing=%u  duplicates=%u\n",
                    static_cast<unsigned>(decoded_packets),
                    static_cast<unsigned>(raw_packets),
                    static_cast<unsigned>(missing_packets),
                    static_cast<unsigned>(duplicate_packets));
        std::printf("         drain_hits=%u  carrier_events=%u\n",
                    static_cast<unsigned>(drain_limit_hits),
                    static_cast<unsigned>(carrier_events));
        std::printf("Remote : control=%s  auto_rx=%s\n",
                    kWirelessControlEnabled ? "enabled" : "disabled",
                    kWirelessControlAutoRx ? "enabled" : "disabled");
        if (!last_morse_text_.empty()) {
            std::printf("Morse  : last=\"%s\"\n", last_morse_text_.c_str());
        }
        if (loop.active) {
            std::printf("Loop   : mode=%s", loopModeName(loop.mode));
            if (loop.mode == LoopMode::Tx) {
                std::printf("  file=%s", loop.file_name.c_str());
                if (loop.infinite) {
                    std::printf("  remaining=inf");
                } else {
                    std::printf("  remaining=%u", static_cast<unsigned>(loop.remaining_iterations));
                }
            } else if (loop.mode == LoopMode::Cw) {
                std::printf("  on_ms=%u  off_ms=%u  power=%u",
                            static_cast<unsigned>(loop.cw_on_ms),
                            static_cast<unsigned>(loop.cw_off_ms),
                            static_cast<unsigned>(loop.power_level));
                if (loop.cw_report_every > 0) {
                    std::printf("  report_every=%u",
                                static_cast<unsigned>(loop.cw_report_every));
                }
            } else if (loop.mode == LoopMode::Morse) {
                std::printf("  text=%s", loop.morse_text.c_str());
            }
            std::printf("  completed=%u\n", static_cast<unsigned>(loop.completed_iterations));
        } else {
            std::printf("Loop   : inactive\n");
        }
        std::printf("====================================================================\n");
    }

    std::string buildStatusJson()
    {
        return AppStatus::buildJson(status_cache_.capture(),
                                    WifiControlConfig::kNodeName,
                                    WifiControlConfig::kNodeName);
    }

    bool dispatchHttpCommand(const std::string& line)
    {
        return dispatchCommand(line, CommandOrigin::Http);
    }

    bool commandFiles()
    {
        printFileTable(listFiles(), selectedFileName());
        return true;
    }

    bool captureFilesystemSnapshot(std::vector<FileInfo>& entries,
                                   size_t& total,
                                   size_t& used)
    {
        if (!filesystem_ready_) {
            std::printf("SPIFFS is not mounted.\n");
            return false;
        }
        if (!takeRadio(pdMS_TO_TICKS(500))) {
            std::printf("Filesystem is busy. Run STOP and try again.\n");
            return false;
        }
        bool scan_ok = false;
        entries = listAllFiles(&scan_ok);
        const esp_err_t result = esp_spiffs_info(nullptr, &total, &used);
        giveRadio();
        if (!scan_ok) {
            std::printf("Could not enumerate every SPIFFS directory entry.\n");
            return false;
        }
        if (result != ESP_OK) {
            std::printf("Could not read SPIFFS usage: %s\n", esp_err_to_name(result));
            return false;
        }
        return true;
    }

    bool printFilesystemInfo()
    {
        std::vector<FileInfo> entries;
        size_t total = 0;
        size_t used = 0;
        if (!captureFilesystemSnapshot(entries, total, used)) {
            return false;
        }

        const size_t free_bytes = used < total ? total - used : 0;
        const uint64_t safe_receive = FileTransfer::safeReceiveCapacity(total, used);
        size_t visible_count = 0;
        size_t hidden_count = 0;
        size_t internal_count = 0;
        uint64_t visible_bytes = 0;
        uint64_t hidden_bytes = 0;
        uint64_t internal_bytes = 0;
        for (const FileInfo& entry : entries) {
            if (ReliableTransferV2::isInternalTransferName(entry.name)) {
                ++internal_count;
                internal_bytes += entry.bytes;
            } else if (!entry.name.empty() && entry.name.front() == '.') {
                ++hidden_count;
                hidden_bytes += entry.bytes;
            } else {
                ++visible_count;
                visible_bytes += entry.bytes;
            }
        }
        const uint64_t listed_bytes = visible_bytes + hidden_bytes + internal_bytes;
        const uint64_t overhead_bytes = used > listed_bytes ? used - listed_bytes : 0;
        std::printf("SPIFFS\n");
        std::printf("  used                 : %u bytes\n", static_cast<unsigned>(used));
        std::printf("  free                 : %u bytes\n", static_cast<unsigned>(free_bytes));
        std::printf("  total                : %u bytes\n", static_cast<unsigned>(total));
        std::printf("  safe receive capacity: %llu bytes (25%% GC reserve)\n",
                    static_cast<unsigned long long>(safe_receive));
        std::printf("  visible files        : %u / %llu bytes\n",
                    static_cast<unsigned>(visible_count),
                    static_cast<unsigned long long>(visible_bytes));
        std::printf("  hidden dot-files     : %u / %llu bytes\n",
                    static_cast<unsigned>(hidden_count),
                    static_cast<unsigned long long>(hidden_bytes));
        std::printf("  internal .part files : %u / %llu bytes\n",
                    static_cast<unsigned>(internal_count),
                    static_cast<unsigned long long>(internal_bytes));
        std::printf("  allocation overhead  : %llu bytes\n",
                    static_cast<unsigned long long>(overhead_bytes));
        return true;
    }

    bool printAllFilesystemFiles()
    {
        std::vector<FileInfo> entries;
        size_t total = 0;
        size_t used = 0;
        if (!captureFilesystemSnapshot(entries, total, used)) {
            return false;
        }
        if (entries.empty()) {
            std::printf("SPIFFS contains no files.\n");
            return true;
        }

        std::printf("All SPIFFS files:\n");
        uint64_t listed_bytes = 0;
        for (const FileInfo& entry : entries) {
            const char* classification =
                ReliableTransferV2::isInternalTransferName(entry.name)
                    ? "internal-partial"
                    : !entry.name.empty() && entry.name.front() == '.'
                          ? "hidden"
                          : "visible";
            std::printf("  [%-16s] %s (%u bytes)\n",
                        classification,
                        entry.name.c_str(),
                        static_cast<unsigned>(entry.bytes));
            listed_bytes += entry.bytes;
        }
        std::printf("Listed %u file(s), %llu logical bytes; SPIFFS reports %u allocated bytes.\n",
                    static_cast<unsigned>(entries.size()),
                    static_cast<unsigned long long>(listed_bytes),
                    static_cast<unsigned>(used));
        return true;
    }

    bool beginFilesystemMutation()
    {
        if (!filesystem_ready_) {
            std::printf("SPIFFS is not mounted.\n");
            return false;
        }
        if (!takeLoop(pdMS_TO_TICKS(50))) {
            std::printf("Could not inspect the TX loop state. Try again.\n");
            return false;
        }
        const bool loop_active = loop_config_.active;
        giveLoop();
        if (loop_active) {
            std::printf("Filesystem is busy with an active loop. Run STOP first.\n");
            return false;
        }
        if (!takeRadio(pdMS_TO_TICKS(500))) {
            std::printf("Filesystem is busy with radio activity. Run STOP and try again.\n");
            return false;
        }
        if (receiver_.active() || incoming_file_.file) {
            giveRadio();
            std::printf("Filesystem is receiving a file. Run STOP before changing files.\n");
            return false;
        }
        return true;
    }

    bool commandFilesystem(const std::vector<std::string>& words)
    {
        if (words.size() < 2) {
            std::printf("Usage: FS INFO | FS LIST ALL | FS DELETE <file> | FS CLEAN PARTIALS | FS FORMAT CONFIRM\n");
            return false;
        }

        const std::string action = uppercaseCopy(words[1]);
        if (action == "INFO") {
            if (words.size() != 2) {
                std::printf("Usage: FS INFO\n");
                return false;
            }
            return printFilesystemInfo();
        }

        if (action == "LIST") {
            if (words.size() != 3 || uppercaseCopy(words[2]) != "ALL") {
                std::printf("Usage: FS LIST ALL\n");
                return false;
            }
            return printAllFilesystemFiles();
        }

        if (action == "DELETE") {
            if (words.size() != 3) {
                std::printf("Usage: FS DELETE <file>\n");
                return false;
            }
            if (!beginFilesystemMutation()) {
                return false;
            }

            FileInfo file{};
            if (!resolveFile(words[2], file)) {
                giveRadio();
                std::printf("File '%s' was not found or is an internal .part file.\n",
                            words[2].c_str());
                return false;
            }

            errno = 0;
            const bool removed = std::remove(buildFilePath(file.name).c_str()) == 0;
            const int remove_errno = errno;
            if (removed && selectedFileName() == file.name) {
                const std::vector<FileInfo> remaining = listFiles();
                if (remaining.empty()) {
                    selectFile(FileInfo{kDefaultFile, 0});
                } else {
                    selectFile(remaining.front());
                }
            }
            giveRadio();

            if (!removed) {
                std::printf("Could not delete %s: errno=%d\n",
                            file.name.c_str(), remove_errno);
                return false;
            }
            std::printf("Deleted %s (%u bytes).\n",
                        file.name.c_str(), static_cast<unsigned>(file.bytes));
            return printFilesystemInfo();
        }

        if (action == "CLEAN") {
            if (words.size() != 3 || uppercaseCopy(words[2]) != "PARTIALS") {
                std::printf("Usage: FS CLEAN PARTIALS\n");
                return false;
            }
            if (!beginFilesystemMutation()) {
                return false;
            }
            cleanupStaleIncomingFiles();
            giveRadio();
            std::printf("Stale .part cleanup complete. Use FS LIST ALL to verify.\n");
            return printFilesystemInfo();
        }

        if (action == "FORMAT") {
            if (words.size() != 3 || uppercaseCopy(words[2]) != "CONFIRM") {
                std::printf("Formatting erases every SPIFFS file. Use: FS FORMAT CONFIRM\n");
                return false;
            }
            if (!beginFilesystemMutation()) {
                return false;
            }

            const esp_err_t result = esp_spiffs_format(nullptr);
            if (result == ESP_OK) {
                incoming_file_ = IncomingFileStorage{};
                resetRxSession();
                selectFile(FileInfo{kDefaultFile, 0});
            }
            giveRadio();

            if (result != ESP_OK) {
                std::printf("SPIFFS format failed: %s\n", esp_err_to_name(result));
                return false;
            }
            std::printf("SPIFFS formatted. All files were erased.\n");
            return printFilesystemInfo();
        }

        std::printf("Usage: FS INFO | FS LIST ALL | FS DELETE <file> | FS CLEAN PARTIALS | FS FORMAT CONFIRM\n");
        return false;
    }

    bool commandSelect(const std::vector<std::string>& words)
    {
        // SELECT only changes the default file used by later transmit (TX)
        // commands. It
        // does not start transmission immediately.
        if (words.size() < 2) {
            std::printf("Usage: SELECT <file>\n");
            return false;
        }

        FileInfo file{};
        if (!resolveFile(words[1], file)) {
            std::printf("File '%s' was not found in SPIFFS.\n", words[1].c_str());
            return false;
        }

        selectFile(file);
        std::printf("Selected %s (%u bytes)\n",
            file.name.c_str(),
            static_cast<unsigned>(file.bytes));
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
                    std::printf("Usage: TX LOOP [count|INF] [file]\n");
                    return false;
                }

                bool infinite = true;
                uint32_t loop_count = 0;
                std::string request = selectedFileName();

                if (words.size() >= 3) {
                    if (parseLoopCountToken(words[2], infinite, loop_count)) {
                        if (words.size() >= 4) {
                            request = words[3];
                        }
                    } else {
                        request = words[2];
                    }
                }

                FileInfo file{};
                if (!resolveFile(request, file)) {
                    std::printf("File '%s' was not found in SPIFFS.\n", request.c_str());
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

                selectFile(file);
                loop_stop_requested_.store(false);
                loop_config_ = LoopConfig{};
                loop_config_.mode = LoopMode::Tx;
                loop_config_.active = true;
                loop_config_.infinite = infinite;
                loop_config_.restore_rx_after_completion =
                    origin == CommandOrigin::Remote && kWirelessControlEnabled && kWirelessControlAutoRx;
                loop_config_.remaining_iterations = loop_count;
                loop_config_.file_name = file.name;
                giveLoop();

                if (infinite) {
                    std::printf("TX loop active for %s (infinite)\n", file.name.c_str());
                } else {
                    std::printf("TX loop active for %s (%u passes)\n",
                                file.name.c_str(),
                                static_cast<unsigned>(loop_count));
                }
                return true;
            }
        }

        // Transmit (TX) either uses the explicitly requested file or the
        // currently selected default file.
        const std::string request = words.size() >= 2 ? words[1] : selectedFileName();
        FileInfo file{};
        if (!resolveFile(request, file)) {
            std::printf("File '%s' was not found in SPIFFS.\n", request.c_str());
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

        selectFile(file);
        loop_stop_requested_.store(false);
        loop_config_ = LoopConfig{};
        loop_config_.mode = LoopMode::Tx;
        loop_config_.active = true;
        loop_config_.infinite = false;
        loop_config_.restore_rx_after_completion =
            origin == CommandOrigin::Remote && kWirelessControlEnabled && kWirelessControlAutoRx;
        loop_config_.remaining_iterations = 1;
        loop_config_.file_name = file.name;
        giveLoop();

        std::printf("TX started for %s. Use STOP to abort.\n", file.name.c_str());
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
            std::printf("Remote commands support the full command set except REMOTE and FS.\n");
            return false;
        }

        uint8_t packet[AudioPacket::kPacketBytes] = {};
        size_t packet_len = 0;
        if (!StreamSync::encodeRemoteCommand(request.c_str(), request.size(), packet, packet_len) ||
            packet_len != AudioPacket::kPacketBytes) {
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
            ok = sendPayloadWithRetry(manager_, packet, AudioPacket::kPacketBytes);
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
                              const std::atomic_bool* stop_requested = nullptr,
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
            if (stop_requested && stop_requested->load()) {
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

        loop_stop_requested_.store(false);
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
        CommandParsing::ChannelRequest request{};
        if (!CommandParsing::parseChannelCommand(words, request)) {
            std::printf("Usage: CHANNEL <0-125> | CHANNEL PREVIEW <0-125>\n");
            return false;
        }

        const uint8_t channel = request.channel;
        if (request.action == CommandParsing::ChannelAction::Preview) {
            // Return before taking the radio lock or changing any transfer state.
            std::printf("Channel %u: %u MHz (nominal center frequency). Radio unchanged.\n"
                        "Use CHANNEL %u to select it.\n",
                        static_cast<unsigned>(channel),
                        static_cast<unsigned>(RadioChannel::frequencyMHz(channel)),
                        static_cast<unsigned>(channel));
            return true;
        }

        // Changing channels is implemented as a full re-boot of the radio so
        // packet mode returns to a known baseline on the new channel while
        // preserving the selected TX power level.
        if (!stopLoopAndWait()) {
            std::printf("Could not stop the current loop cleanly.\n");
            return false;
        }

        if (!takeRadio()) {
            std::printf("Radio is busy.\n");
            return false;
        }

        discardIncomingFileTransfer("Discarding partial RX file before channel change");
        const bool ok = manager_.boot(channel);
        const RadioStatus status = manager_.status();
        giveRadio();

        if (!ok) {
            std::printf("Could not switch channel. state=%s fault=%d\n",
                        RadioManager::stateName(status.state),
                        status.last_fault);
            return false;
        }

        std::printf("Radio reinitialized on channel %u (%u MHz)\n",
                    static_cast<unsigned>(status.channel),
                    static_cast<unsigned>(RadioChannel::frequencyMHz(status.channel)));
        return true;
    }

    bool commandPower(const std::vector<std::string>& words)
    {
        if (words.size() < 2) {
            std::printf("Usage: POWER <0-3>\n");
            return false;
        }

        uint8_t power_level = 0;
        if (!parseUint8Arg(words[1], 0, 3, power_level)) {
            std::printf("TX power level must be in the range 0-3.\n");
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

        bool ok = ensureStandbyLocked();
        if (ok) {
            ok = manager_.setPowerLevel(power_level);
        }

        const RadioStatus status = manager_.status();
        giveRadio();

        if (!ok) {
            std::printf("Could not set TX power. state=%s fault=%d\n",
                        RadioManager::stateName(status.state),
                        status.last_fault);
            return false;
        }

        std::printf("Packet TX power set to %u on channel %u\n",
                    static_cast<unsigned>(power_level),
                    static_cast<unsigned>(status.channel));
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

            uint8_t channel = 76;
            if (!takeRadio(pdMS_TO_TICKS(100))) {
                std::printf("Radio is busy; could not read the current channel.\n");
                return false;
            }
            channel = manager_.status().channel;
            giveRadio();
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

            loop_stop_requested_.store(false);
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
            const std::string path = buildFilePath(loop.file_name);
            ok = sendDataFile(
                manager_,
                transfer_service_,
                path.c_str(),
                &loop_stop_requested_,
                &last_tx_report_,
                &DemoConsoleApp::publishTransferStatusEntry,
                this);
            status = manager_.status();
        }
        giveRadio();

        const bool stopped = loop_stop_requested_.load();
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
            ESP_LOGI(TAG, "TX pass complete for %s", loop.file_name.c_str());
        } else if (stopped) {
            ESP_LOGI(TAG, "TX stopped for %s", loop.file_name.c_str());
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

        const bool stopped = loop_stop_requested_.load();
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

        if (loop_stop_requested_.load() && takeLoop(pdMS_TO_TICKS(50))) {
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

        const bool stopped = loop_stop_requested_.load();
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

    bool sendProtocolResponseLocked(const ProtocolV2::Frame& response)
    {
        if (manager_.status().state != RadioState::RxListening ||
            !manager_.leaveRx()) {
            return false;
        }
        const bool sent = manager_.sendPayload(response.data(), response.size());
        const bool resumed = manager_.enterRx();
        return sent && resumed;
    }

    void processRxPayload(const uint8_t* payload, size_t len)
    {
        const uint64_t now_ms = monotonicMilliseconds();
        ProtocolV2::Packet incoming{};
        const ProtocolV2::DecodeStatus decoded_status =
            ProtocolV2::decode(payload, len, incoming);
        const uint32_t expected_before = receiver_.expectedSequence();
        const bool was_complete = receiver_.complete();
        ProtocolV2::Frame response{};
        const ReliableTransferV2::ReceiverEvent event = receiver_.onFrame(
            payload, len, now_ms, response);

        if (decoded_status == ProtocolV2::DecodeStatus::Ok &&
            incoming.type == ProtocolV2::PacketType::Start &&
            receiver_.active() &&
            receiver_.transferId() == incoming.transfer_id &&
            active_api_receive_id_ != incoming.transfer_id) {
            active_api_receive_id_ = incoming.transfer_id;
            last_rx_progress_percent_ = 0;
            rx_transfer_started_ms_ = now_ms;
            transfer_service_.reportReceiveStarted(
                receiver_.transferId(),
                receiver_.totalSize(),
                receiver_.totalPackets(),
                receiver_.expectedCrc32(),
                now_ms);
        }

        if (event == ReliableTransferV2::ReceiverEvent::Ignored) {
            ++raw_rx_packet_count_;
#if RF3_VERBOSE_RX_LOG
            ESP_LOGW(TAG,
                     "RX ignored non-v2/stale frame decode=%u len=%u",
                     static_cast<unsigned>(decoded_status),
                     static_cast<unsigned>(len));
#endif
            return;
        }

        if (!sendProtocolResponseLocked(response)) {
            ESP_LOGW(TAG,
                     "Protocol v2 response transmit failed transfer=%08lX state=%s",
                     static_cast<unsigned long>(receiver_.transferId()),
                     ReliableTransferV2::receiverStateName(receiver_.state()));
        }

        if (event == ReliableTransferV2::ReceiverEvent::DataAccepted) {
            ++decoded_rx_packet_count_;
            transfer_service_.reportReceiveProgress(
                receiver_.transferId(),
                receiver_.acceptedBytes(),
                incoming.sequence,
                now_ms);
            const uint8_t progress_percent = FileTransfer::progressMilestonePercent(
                receiver_.acceptedBytes(), receiver_.totalSize());
            if (progress_percent > last_rx_progress_percent_) {
                last_rx_progress_percent_ = progress_percent;
                const uint64_t elapsed_ms = now_ms >= rx_transfer_started_ms_
                                                ? now_ms - rx_transfer_started_ms_ : 0;
                const uint32_t rate_bps = elapsed_ms == 0
                    ? 0
                    : static_cast<uint32_t>(
                          (static_cast<uint64_t>(receiver_.acceptedBytes()) * 1000u) /
                          elapsed_ms);
                ESP_LOGI(TAG,
                         "RX progress id=%08lX %u%% | bytes=%lu/%lu | packets=%lu/%lu | rate=%lu B/s",
                         static_cast<unsigned long>(receiver_.transferId()),
                         static_cast<unsigned>(progress_percent),
                         static_cast<unsigned long>(receiver_.acceptedBytes()),
                         static_cast<unsigned long>(receiver_.totalSize()),
                         static_cast<unsigned long>(receiver_.acceptedPackets()),
                         static_cast<unsigned long>(receiver_.totalPackets()),
                         static_cast<unsigned long>(rate_bps));
            }
#if RF3_VERBOSE_RX_LOG
            if (incoming.sequence == 0 ||
                (receiver_.acceptedPackets() % 64u) == 0) {
                ESP_LOGI(TAG,
                         "Protocol v2 RX DATA id=%08lX seq=%u bytes=%u total=%lu/%lu",
                         static_cast<unsigned long>(receiver_.transferId()),
                         static_cast<unsigned>(incoming.sequence),
                         static_cast<unsigned>(incoming.payload_length),
                         static_cast<unsigned long>(receiver_.acceptedBytes()),
                         static_cast<unsigned long>(receiver_.totalSize()));
            }
#endif
            return;
        }

        if (event == ReliableTransferV2::ReceiverEvent::Duplicate) {
            ++raw_rx_packet_count_;
            ++duplicate_rx_packet_count_;
            return;
        }

        if (event == ReliableTransferV2::ReceiverEvent::Completed) {
            if (!was_complete) {
                ++saved_rx_file_count_;
                saved_rx_byte_count_ += receiver_.acceptedBytes();
                ESP_LOGI(TAG,
                         "Protocol v2 verified and published %s id=%08lX bytes=%lu packets=%lu crc32=%08lX",
                         incoming_file_.final_name.c_str(),
                         static_cast<unsigned long>(receiver_.transferId()),
                         static_cast<unsigned long>(receiver_.acceptedBytes()),
                         static_cast<unsigned long>(receiver_.acceptedPackets()),
                         static_cast<unsigned long>(receiver_.calculatedCrc32()));
                transfer_service_.reportReceiveCompleted(
                    receiver_.transferId(),
                    buildFilePath(incoming_file_.final_name),
                    now_ms);
                active_api_receive_id_ = 0;
            }
            return;
        }

        ++raw_rx_packet_count_;
        if (event == ReliableTransferV2::ReceiverEvent::ResponseReady) {
            if (decoded_status == ProtocolV2::DecodeStatus::Ok &&
                incoming.type == ProtocolV2::PacketType::Data &&
                static_cast<uint32_t>(incoming.sequence) > expected_before) {
                missing_rx_packet_count_ +=
                    static_cast<uint32_t>(incoming.sequence) - expected_before;
            }
#if RF3_VERBOSE_RX_LOG
            ESP_LOGI(TAG,
                     "Protocol v2 RX control id=%08lX type=%s state=%s expected=%lu",
                     static_cast<unsigned long>(receiver_.transferId()),
                     decoded_status == ProtocolV2::DecodeStatus::Ok
                         ? ProtocolV2::packetTypeName(incoming.type) : "INVALID",
                     ReliableTransferV2::receiverStateName(receiver_.state()),
                     static_cast<unsigned long>(receiver_.expectedSequence()));
#endif
            return;
        }

        ESP_LOGE(TAG,
                 "Protocol v2 RX terminal event=%u id=%08lX state=%s error=%s bytes=%lu/%lu seq=%lu cleanup_failed=%s",
                 static_cast<unsigned>(event),
                 static_cast<unsigned long>(receiver_.transferId()),
                 ReliableTransferV2::receiverStateName(receiver_.state()),
                 ProtocolV2::errorName(receiver_.error()),
                 static_cast<unsigned long>(receiver_.acceptedBytes()),
                 static_cast<unsigned long>(receiver_.totalSize()),
                 static_cast<unsigned long>(receiver_.expectedSequence()),
                  receiver_.cleanupFailed() ? "true" : "false");
        transfer_service_.reportReceiveFailed(
            receiver_.transferId(), receiver_.error(), now_ms);
        active_api_receive_id_ = 0;
    }

    void rxTask()
    {
        // This background task is intentionally conservative:
        // - it tries to grab the mutex briefly
        // - it refreshes idle diagnostics at a bounded cadence
        // - it drains any queued packets before releasing the radio again
        // - it saves accepted payload streams into SPIFFS as completed files
        std::array<uint8_t, AudioPacket::kPacketBytes> payload{};
        constexpr uint64_t kIdleDiagnosticPeriodMs = 250;
        uint64_t last_diagnostic_ms = 0;

        while (true) {
            std::string remote_command;

            if (takeRadio(pdMS_TO_TICKS(10))) {
                if (manager_.status().state == RadioState::RxListening) {
                    manager_.refreshSnapshot();
                    last_diagnostic_ms = monotonicMilliseconds();
                    const RadioStatus snapshot = manager_.status();

                    if (snapshot.carrier_detected && !last_carrier_detected_) {
                        ++carrier_event_count_;
                    }
                    last_carrier_detected_ = snapshot.carrier_detected;

                    const RxDrain::DrainResult drain_result = RxDrain::drainPending(
                        [&]() {
                            return manager_.hasPendingRx();
                        },
                        [&]() {
                            size_t out_len = 0;
                            if (manager_.receivePayload(payload.data(), payload.size(), out_len)) {
                                std::string_view remote_view;
                                if (kWirelessControlEnabled &&
                                    StreamSync::decodeRemoteCommand(payload.data(), out_len, remote_view)) {
                                    remote_command.assign(remote_view.data(), remote_view.size());
                                    return RxDrain::StepResult::Stop;
                                }

                                processRxPayload(payload.data(), out_len);
                                return RxDrain::StepResult::Processed;
                            }

                            const RadioStatus status = manager_.status();
                            ESP_LOGW(TAG, "RX read failed, state=%s fault=%d",
                                     RadioManager::stateName(status.state),
                                     status.last_fault);
                            return RxDrain::StepResult::Failed;
                        },
                        RxDrain::kDefaultMaxPacketsPerPoll);

                    if (drain_result.guard_exhausted) {
                        ++rx_drain_limit_hit_count_;
                        if (rx_drain_limit_hit_count_ == 1 ||
                            (rx_drain_limit_hit_count_ % 64u) == 0) {
                            ESP_LOGW(TAG,
                                     "RX drain guard hits=%lu limit=%u",
                                     static_cast<unsigned long>(rx_drain_limit_hit_count_),
                                     static_cast<unsigned>(drain_result.processed));
                        }
                    }
                } else {
                    last_carrier_detected_ = false;
                    const uint64_t now_ms = monotonicMilliseconds();
                    if (now_ms - last_diagnostic_ms >= kIdleDiagnosticPeriodMs) {
                        // Keep idle disconnect/register diagnostics current without
                        // making a status request wait on SPI or the radio lock.
                        manager_.refreshSnapshot();
                        last_diagnostic_ms = now_ms;
                    }
                }

                ProtocolV2::Frame timeout_response{};
                if (receiver_.tick(monotonicMilliseconds(), timeout_response) ==
                    ReliableTransferV2::ReceiverEvent::TimedOut) {
                    transfer_service_.reportReceiveFailed(
                        receiver_.transferId(),
                        receiver_.error(),
                        monotonicMilliseconds());
                    active_api_receive_id_ = 0;
                    (void)sendProtocolResponseLocked(timeout_response);
                    ESP_LOGE(TAG,
                             "Protocol v2 RX timed out transfer=%08lX after %lu ms cleanup_failed=%s",
                             static_cast<unsigned long>(receiver_.transferId()),
                             static_cast<unsigned long>(receiver_.timeoutMs()),
                             receiver_.cleanupFailed() ? "true" : "false");
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
        if (command == "FS") {
            return commandFilesystem(words);
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
        if (command == "POWER") {
            return commandPower(words);
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
            CommandParsing::ChannelRequest request{};
            const bool channel_preview =
                CommandParsing::parseChannelCommand(splitWords(line), request) &&
                request.action == CommandParsing::ChannelAction::Preview;
            if (!channel_preview) {
                tryResumeWirelessRx("Remote control RX resumed");
            }
        }
        giveCommand();
        return ok;
    }

    RadioManager& manager_;
    FileTransfer::Service transfer_service_;  // Subsystem-facing Protocol v2 transfer API.
    SemaphoreHandle_t radio_mutex_ = nullptr;  // Serializes all radio access.
    TaskHandle_t rx_task_ = nullptr;           // Background receive (RX) polling task.
    SemaphoreHandle_t loop_mutex_ = nullptr;   // Guards background TX/CW loop configuration.
    TaskHandle_t loop_task_ = nullptr;         // Background TX/CW loop worker.
    TaskHandle_t wifi_control_task_ = nullptr; // Starts HTTP control off the event-task stack.
    SemaphoreHandle_t command_mutex_ = nullptr;  // Serializes local and remote command dispatch.
    StatusMutex status_mutex_{};              // Guards only cached, owned status values.
    AppStatus::SnapshotCache<StatusMutex> status_cache_{status_mutex_};
    std::string last_morse_text_;              // Most recent MORSE text for STATUS output.
    bool last_carrier_detected_ = false;      // Edge detector for RPD logging while in RX.
    uint32_t carrier_event_count_ = 0;        // Number of distinct RPD-high events seen while listening.
    IncomingFileStorage incoming_file_{};     // SPIFFS adapter state for the current RX partial file.
    ReliableTransferV2::ReceiverSession receiver_;  // Verified Protocol v2 RX session.
    uint32_t active_api_receive_id_ = 0;       // Receiver transfer mirrored into the subsystem API.
    uint8_t last_rx_progress_percent_ = 0;     // Last reported accepted-byte milestone.
    uint64_t rx_transfer_started_ms_ = 0;      // Monotonic start used for RX progress rate.
    ProtocolTransferReport last_tx_report_{}; // Last reliable sender state for STATUS.
    uint32_t decoded_rx_packet_count_ = 0;    // Payloads accepted as in-order stream data.
    uint32_t raw_rx_packet_count_ = 0;        // Payloads that were received but not accepted as stream data.
    uint32_t missing_rx_packet_count_ = 0;    // Sequence slots skipped by tolerated forward gaps.
    uint32_t duplicate_rx_packet_count_ = 0;  // Duplicate DATA packets re-ACKed without rewriting.
    uint32_t rx_drain_limit_hit_count_ = 0;   // Service passes that stopped at the bounded drain cap.
    uint32_t saved_rx_file_count_ = 0;        // Completed files written to SPIFFS in the current RX session.
    uint32_t saved_rx_byte_count_ = 0;        // Total bytes written across saved RX files in the current RX session.
    LoopConfig loop_config_{};
    std::atomic_bool loop_stop_requested_{false};
    EventGroupHandle_t wifi_event_group_ = nullptr;
    esp_netif_t* wifi_netif_ = nullptr;
    httpd_handle_t http_server_ = nullptr;
    esp_event_handler_instance_t wifi_event_handler_ = nullptr;
    esp_event_handler_instance_t ip_event_handler_ = nullptr;
    bool wifi_connected_ = false;
    bool filesystem_ready_ = false;
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
              HardwareProfile::kSelectedName,
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
