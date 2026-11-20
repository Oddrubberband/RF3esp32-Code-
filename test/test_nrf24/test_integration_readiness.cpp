#include <array>
#include <cstddef>
#include <cstdint>

#include <unity.h>

#include "file_transfer_service.hpp"
#include "hardware_profile.hpp"
#include "transfer_rate_limiter.hpp"
#include "runtime_diagnostics.hpp"
#include "task_timing.hpp"
#include "host_connection_protocol.hpp"

namespace {

uint32_t fixedTransferId(void*)
{
    return 0x12345678u;
}

struct CompletionCapture {
    uint32_t calls = 0;
    FileTransfer::ReceiveCompletion completion{};
};

void captureCompletion(void* context,
                       const FileTransfer::ReceiveCompletion& completion)
{
    CompletionCapture& capture = *static_cast<CompletionCapture*>(context);
    ++capture.calls;
    capture.completion = completion;
}

void test_fileTransfer_memory_source_starts_binary_transfer(void)
{
    const std::array<uint8_t, 5> bytes{{0x00, 0xFF, 0x55, 0x80, 0x01}};
    FileTransfer::BoundedMemoryDataSource source(bytes.data(), bytes.size());
    FileTransfer::Service service(&fixedTransferId);
    FileTransfer::TransferMetadata metadata{};
    TEST_ASSERT_TRUE(metadata.logical_filename.set("sample.u8"));
    TEST_ASSERT_TRUE(metadata.media_type.set("audio/x-unsigned-8bit-pcm"));
    metadata.has_expected_length = true;
    metadata.expected_length = bytes.size();

    const FileTransfer::StartResult started = service.startTransfer(source, metadata, 10);
    TEST_ASSERT_TRUE(static_cast<bool>(started));
    TEST_ASSERT_EQUAL_HEX32(0x12345678u, started.transfer_id);

    FileTransfer::TransferStatus status{};
    TEST_ASSERT_TRUE(service.getTransferStatus(started.transfer_id, status, 12));
    TEST_ASSERT_EQUAL(static_cast<int>(FileTransfer::Role::Sender),
                      static_cast<int>(status.role));
    TEST_ASSERT_EQUAL_UINT32(bytes.size(), status.total_bytes);
    TEST_ASSERT_EQUAL_UINT32(1, status.total_packets);
    TEST_ASSERT_EQUAL_STRING("sample.u8", status.metadata.logical_filename.c_str());
}

void test_fileTransfer_memory_source_enforces_explicit_bound(void)
{
    const std::array<uint8_t, 8> bytes{};
    FileTransfer::BoundedMemoryDataSource source(bytes.data(), bytes.size(), 4);
    FileTransfer::Service service(&fixedTransferId);
    const FileTransfer::StartResult started =
        service.startTransfer(source, FileTransfer::TransferMetadata{}, 10);
    TEST_ASSERT_FALSE(static_cast<bool>(started));
    TEST_ASSERT_EQUAL(static_cast<int>(FileTransfer::StartCode::ReadFailed),
                      static_cast<int>(started.code));
}

void test_fileTransfer_expected_length_mismatch_is_rejected(void)
{
    const std::array<uint8_t, 3> bytes{{1, 2, 3}};
    FileTransfer::BoundedMemoryDataSource source(bytes.data(), bytes.size());
    FileTransfer::Service service(&fixedTransferId);
    FileTransfer::TransferMetadata metadata{};
    metadata.has_expected_length = true;
    metadata.expected_length = 4;
    const FileTransfer::StartResult started = service.startTransfer(source, metadata, 5);
    TEST_ASSERT_EQUAL(static_cast<int>(FileTransfer::StartCode::LengthMismatch),
                      static_cast<int>(started.code));
}

void test_fileTransfer_can_retry_after_source_preparation_failure(void)
{
    const std::array<uint8_t, 2> bytes{{1, 2}};
    FileTransfer::BoundedMemoryDataSource invalid(bytes.data(), bytes.size(), 1);
    FileTransfer::BoundedMemoryDataSource valid(bytes.data(), bytes.size());
    FileTransfer::Service service(&fixedTransferId);
    TEST_ASSERT_FALSE(static_cast<bool>(
        service.startTransfer(invalid, FileTransfer::TransferMetadata{}, 1)));
    TEST_ASSERT_TRUE(static_cast<bool>(
        service.startTransfer(valid, FileTransfer::TransferMetadata{}, 2)));
}

void test_fileTransfer_cancel_targets_active_transfer_id(void)
{
    const std::array<uint8_t, 2> bytes{{1, 2}};
    FileTransfer::BoundedMemoryDataSource source(bytes.data(), bytes.size());
    FileTransfer::Service service(&fixedTransferId);
    const FileTransfer::StartResult started =
        service.startTransfer(source, FileTransfer::TransferMetadata{}, 10);
    TEST_ASSERT_TRUE(static_cast<bool>(started));
    TEST_ASSERT_FALSE(service.cancelTransfer(0xABCDEF01u, 11));
    TEST_ASSERT_TRUE(service.cancelTransfer(started.transfer_id, 12));
}

void test_fileTransfer_receive_handler_runs_after_verified_publication(void)
{
    FileTransfer::Service service;
    CompletionCapture capture{};
    service.registerReceiveHandler(&captureCompletion, &capture);
    service.reportReceiveStarted(7, 100, 5, 0xAABBCCDDu, 1000);
    service.reportReceiveProgress(7, 40, 2, 1010);
    TEST_ASSERT_EQUAL_UINT32(0, capture.calls);
    FileTransfer::TransferStatus progress{};
    TEST_ASSERT_TRUE(service.getTransferStatus(7, progress, 1010));
    TEST_ASSERT_EQUAL_UINT32(40, progress.bytes_transferred);
    TEST_ASSERT_EQUAL_UINT32(2, progress.current_sequence);
    service.reportReceiveCompleted(7, "/spiffs/rx_00000007.bin", 1050);
    TEST_ASSERT_EQUAL_UINT32(1, capture.calls);
    TEST_ASSERT_EQUAL_UINT32(100, capture.completion.status.bytes_transferred);
    TEST_ASSERT_EQUAL_UINT32(4, capture.completion.status.current_sequence);
    TEST_ASSERT_EQUAL_STRING("/spiffs/rx_00000007.bin",
                             capture.completion.status.final_published_path.c_str());
    TEST_ASSERT_EQUAL_UINT64(50, capture.completion.status.elapsed_ms);
}

void test_generic_transfer_default_rate_limit_is_disabled(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, TransferRateLimiter::delayMilliseconds(28, 0));
    TEST_ASSERT_EQUAL_UINT32(4, TransferRateLimiter::delayMilliseconds(28, 8000));
}

void test_hardware_profiles_preserve_distinct_supported_pinouts(void)
{
    constexpr HardwareProfile::Pins pcb =
        HardwareProfile::pinsFor(HardwareProfile::Id::CustomPcb);
    constexpr HardwareProfile::Pins dev =
        HardwareProfile::pinsFor(HardwareProfile::Id::Esp32Devboard);
    TEST_ASSERT_EQUAL_INT(17, pcb.ce);
    TEST_ASSERT_EQUAL_INT(27, pcb.csn);
    TEST_ASSERT_EQUAL_INT(16, pcb.irq);
    TEST_ASSERT_EQUAL_INT(27, dev.ce);
    TEST_ASSERT_EQUAL_INT(5, dev.csn);
    TEST_ASSERT_EQUAL_INT(26, dev.irq);
    TEST_ASSERT_EQUAL_INT(pcb.sck, dev.sck);
    TEST_ASSERT_EQUAL_INT(pcb.mosi, dev.mosi);
    TEST_ASSERT_EQUAL_INT(pcb.miso, dev.miso);
}

void test_diagnostic_totals_saturate_and_json_stays_bounded()
{
    RuntimeDiagnostics::Counters d;
    d.tx_ds.add(UINT32_MAX);
    d.tx_ds.add(9);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, d.tx_ds.value());
    d.rx_poll.record(50);
    d.rx_poll.record(100);
    TEST_ASSERT_EQUAL_UINT32(2, d.rx_poll.count.value());
    TEST_ASSERT_EQUAL_UINT32(150, d.rx_poll.sum_us.value());
    TEST_ASSERT_EQUAL_UINT32(100, d.rx_poll.max_us.value());
    d.storage_write.record(UINT64_MAX);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, d.storage_write.max_us.value());
    const auto json = RuntimeDiagnostics::json(d, "esp32-devboard", 1000);
    TEST_ASSERT_TRUE(json.size() < RuntimeDiagnostics::kJsonCapacity);
    TEST_ASSERT_TRUE(json.find("\"build\":\"RF3-CBT-R4-20261119\"") != std::string::npos);
    TEST_ASSERT_TRUE(json.find("\"tx_ds\":4294967295") != std::string::npos);
    TEST_ASSERT_TRUE(json.find("\"type\":\"diag\"") != std::string::npos);
    const auto hello = HostConnectionProtocol::hello("rf3-pcb", "custom-pcb", 2, true, RuntimeDiagnostics::kBuild);
    TEST_ASSERT_TRUE(hello.find("\"diagnostics\":true") != std::string::npos);
    TEST_ASSERT_TRUE(hello.find(RuntimeDiagnostics::kBuild) != std::string::npos);
}
void test_sender_diagnostics_count_only_rejected_responses_and_expired_data_ack()
{
    const std::array<uint8_t, 40> bytes{};
    FileTransfer::BoundedMemoryDataSource source(bytes.data(), bytes.size());
    FileTransfer::Service service(&fixedTransferId);
    TEST_ASSERT_TRUE(static_cast<bool>(service.startTransfer(source, {}, 0)));
    auto& sender = service.transportSender();
    auto& d = RuntimeDiagnostics::global();
    const auto timeouts = d.ack_timeouts.value();
    const auto rejected = d.rejected_responses.value();
    ProtocolV2::Frame frame{};
    TEST_ASSERT_TRUE(sender.outboundFrame(frame));
    TEST_ASSERT_TRUE(sender.noteFrameSent(0));
    ProtocolV2::Packet ready{};
    ready.type = ProtocolV2::PacketType::Ready;
    ready.transfer_id = sender.transferId();
    ready.accepted = true;
    TEST_ASSERT_TRUE(ProtocolV2::encode(ready, frame));
    TEST_ASSERT_EQUAL(static_cast<int>(ReliableTransferV2::SenderEvent::OutboundReady),
                      static_cast<int>(sender.onFrame(frame.data(), frame.size(), 1)));
    TEST_ASSERT_TRUE(sender.outboundFrame(frame));
    TEST_ASSERT_TRUE(sender.noteFrameSent(2));
    frame[0] = 0;
    TEST_ASSERT_EQUAL(static_cast<int>(ReliableTransferV2::SenderEvent::Ignored),
                      static_cast<int>(sender.onFrame(frame.data(), frame.size(), 3)));
    TEST_ASSERT_EQUAL_UINT32(rejected + 1, d.rejected_responses.value());
    TEST_ASSERT_EQUAL(static_cast<int>(ReliableTransferV2::SenderEvent::NoChange),
                      static_cast<int>(sender.tick(1001)));
    TEST_ASSERT_EQUAL_UINT32(timeouts, d.ack_timeouts.value());
    TEST_ASSERT_EQUAL(static_cast<int>(ReliableTransferV2::SenderEvent::OutboundReady),
                      static_cast<int>(sender.tick(1002)));
    TEST_ASSERT_EQUAL_UINT32(timeouts + 1, d.ack_timeouts.value());
}

void test_service_wait_conversions_represent_two_ms_at_configured_tick_rate()
{
    TEST_ASSERT_EQUAL_UINT32(2, TaskTiming::nonzeroTicks(2, 1000));
    TEST_ASSERT_EQUAL_UINT32(1, TaskTiming::nonzeroTicks(2, 100));
    TEST_ASSERT_EQUAL_UINT32(1, TaskTiming::nonzeroTicks(0, 1000));
    TEST_ASSERT_EQUAL_UINT32(10, TaskTiming::nonzeroTicks(10, 1000));
}

}  // namespace

void runIntegrationReadinessTests()
{
    RUN_TEST(test_fileTransfer_memory_source_starts_binary_transfer);
    RUN_TEST(test_fileTransfer_memory_source_enforces_explicit_bound);
    RUN_TEST(test_fileTransfer_expected_length_mismatch_is_rejected);
    RUN_TEST(test_fileTransfer_can_retry_after_source_preparation_failure);
    RUN_TEST(test_fileTransfer_cancel_targets_active_transfer_id);
    RUN_TEST(test_fileTransfer_receive_handler_runs_after_verified_publication);
    RUN_TEST(test_generic_transfer_default_rate_limit_is_disabled);
    RUN_TEST(test_hardware_profiles_preserve_distinct_supported_pinouts);
    RUN_TEST(test_diagnostic_totals_saturate_and_json_stays_bounded);
    RUN_TEST(test_service_wait_conversions_represent_two_ms_at_configured_tick_rate);
    RUN_TEST(test_sender_diagnostics_count_only_rejected_responses_and_expired_data_ack);
}
