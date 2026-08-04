#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <unity.h>

#include "file_receiver.hpp"

namespace {

using FileReceiver::Failure;
using FileReceiver::Packet;
using FileReceiver::PrepareResult;
using FileReceiver::Result;
using FileReceiver::StorageCallbacks;
using FileReceiver::Transfer;

struct FakeStorage {
    PrepareResult prepare_result = PrepareResult::Ready;
    size_t write_limit = std::numeric_limits<size_t>::max();
    bool close_ok = true;
    bool publish_ok = true;
    bool remove_ok = true;
    bool capture_bytes = true;
    bool open = false;
    bool partial_exists = false;
    uint16_t stream_id = 0;
    uint32_t prepare_calls = 0;
    uint32_t write_calls = 0;
    uint32_t close_calls = 0;
    uint32_t publish_calls = 0;
    uint32_t remove_calls = 0;
    uint32_t bytes_written = 0;
    std::vector<uint8_t> partial;
    std::vector<uint8_t> published;
    std::vector<uint8_t> existing_completed{0xC0, 0xFF, 0xEE};
};

PrepareResult fakePrepare(void* context, uint16_t stream_id)
{
    FakeStorage* storage = static_cast<FakeStorage*>(context);
    ++storage->prepare_calls;
    storage->stream_id = stream_id;
    if (storage->prepare_result == PrepareResult::Ready) {
        storage->open = true;
        storage->partial_exists = true;
        storage->partial.clear();
        storage->bytes_written = 0;
    } else if (storage->prepare_result == PrepareResult::CleanupFailed) {
        storage->partial_exists = true;
    }
    return storage->prepare_result;
}

size_t fakeWrite(void* context, const uint8_t* data, size_t length)
{
    FakeStorage* storage = static_cast<FakeStorage*>(context);
    ++storage->write_calls;
    if (!storage->open || !data) {
        return 0;
    }

    const size_t count = std::min(length, storage->write_limit);
    if (storage->capture_bytes) {
        storage->partial.insert(storage->partial.end(), data, data + count);
    }
    storage->bytes_written += static_cast<uint32_t>(count);
    return count;
}

bool fakeClose(void* context)
{
    FakeStorage* storage = static_cast<FakeStorage*>(context);
    ++storage->close_calls;
    storage->open = false;
    return storage->close_ok;
}

bool fakePublish(void* context)
{
    FakeStorage* storage = static_cast<FakeStorage*>(context);
    ++storage->publish_calls;
    if (!storage->publish_ok) {
        return false;
    }
    if (storage->capture_bytes) {
        storage->published = storage->partial;
    }
    storage->partial_exists = false;
    return true;
}

bool fakeRemovePartial(void* context)
{
    FakeStorage* storage = static_cast<FakeStorage*>(context);
    ++storage->remove_calls;
    if (!storage->remove_ok) {
        return false;
    }
    storage->open = false;
    storage->partial_exists = false;
    storage->partial.clear();
    return true;
}

StorageCallbacks fakeCallbacks()
{
    return {
        &fakePrepare,
        &fakeWrite,
        &fakeClose,
        &fakePublish,
        &fakeRemovePartial,
    };
}

Packet packet(uint16_t sequence, uint8_t length, uint8_t flags)
{
    return {sequence, length, flags};
}

void assertVectorEquals(const std::vector<uint8_t>& expected,
                        const std::vector<uint8_t>& actual)
{
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(expected.size()),
                             static_cast<uint32_t>(actual.size()));
    for (size_t index = 0; index < expected.size(); ++index) {
        TEST_ASSERT_EQUAL_UINT8(expected[index], actual[index]);
    }
}

Result sendFixture(Transfer& transfer,
                   const std::vector<uint8_t>& data,
                   uint64_t start_time = 100)
{
    TEST_ASSERT_FALSE(data.empty());
    Result result = Result::NoActiveSession;
    size_t offset = 0;
    uint16_t sequence = 0;
    while (offset < data.size()) {
        const size_t count = std::min<size_t>(AudioPacket::kAudioBytesPerPacket,
                                              data.size() - offset);
        const bool first = sequence == 0;
        const bool last = offset + count == data.size();
        uint8_t flags = first ? AudioPacket::kFirst : 0;
        if (last) {
            flags |= AudioPacket::kLast;
        }
        result = transfer.accept(packet(sequence, static_cast<uint8_t>(count), flags),
                                 data.data() + offset,
                                 start_time + sequence + 1u);
        offset += count;
        ++sequence;
    }
    return result;
}

void test_receiver_one_packet_transfer_publishes_exact_bytes()
{
    FakeStorage storage;
    Transfer transfer(&storage, fakeCallbacks());
    const std::vector<uint8_t> bytes{0x10, 0x00, 0xFF};

    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Started),
                          static_cast<int>(transfer.start(7, 100)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Completed),
                          static_cast<int>(sendFixture(transfer, bytes, 100)));
    assertVectorEquals(bytes, storage.published);
    TEST_ASSERT_EQUAL_UINT32(3, transfer.acceptedBytes());
    TEST_ASSERT_EQUAL_UINT32(1, transfer.acceptedPackets());
}

void test_receiver_multi_packet_transfer_is_contiguous_and_binary_safe()
{
    FakeStorage storage;
    Transfer transfer(&storage, fakeCallbacks());
    std::vector<uint8_t> bytes(57);
    for (size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<uint8_t>(index);
    }

    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Started),
                          static_cast<int>(transfer.start(8, 0)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Completed),
                          static_cast<int>(sendFixture(transfer, bytes, 0)));
    assertVectorEquals(bytes, storage.published);
    TEST_ASSERT_EQUAL_UINT32(3, transfer.acceptedPackets());
}

void test_receiver_exact_28_byte_packet_completes()
{
    FakeStorage storage;
    Transfer transfer(&storage, fakeCallbacks());
    const std::vector<uint8_t> bytes(28, 0xA5);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Started),
                          static_cast<int>(transfer.start(9, 0)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Completed),
                          static_cast<int>(sendFixture(transfer, bytes, 0)));
    assertVectorEquals(bytes, storage.published);
}

void test_receiver_all_byte_values_round_trip()
{
    FakeStorage storage;
    Transfer transfer(&storage, fakeCallbacks());
    std::vector<uint8_t> bytes(256);
    for (size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<uint8_t>(index);
    }
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Started),
                          static_cast<int>(transfer.start(10, 0)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Completed),
                          static_cast<int>(sendFixture(transfer, bytes, 0)));
    assertVectorEquals(bytes, storage.published);
}

void test_receiver_maximum_sequence_completes_without_large_allocation()
{
    FakeStorage storage;
    storage.capture_bytes = false;
    Transfer transfer(&storage, fakeCallbacks());
    uint8_t payload[AudioPacket::kAudioBytesPerPacket] = {};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Started),
                          static_cast<int>(transfer.start(11, 0)));

    for (uint32_t sequence = 0; sequence <= UINT16_MAX; ++sequence) {
        uint8_t flags = sequence == 0 ? AudioPacket::kFirst : 0;
        if (sequence == UINT16_MAX) {
            flags |= AudioPacket::kLast;
        }
        const Result result = transfer.accept(
            packet(static_cast<uint16_t>(sequence),
                   AudioPacket::kAudioBytesPerPacket,
                   flags),
            payload,
            sequence + 1u);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(sequence == UINT16_MAX
                                                   ? Result::Completed
                                                   : Result::Accepted),
                              static_cast<int>(result));
    }

    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(UINT16_MAX) + 1u,
                             transfer.acceptedPackets());
    TEST_ASSERT_EQUAL_UINT32((static_cast<uint32_t>(UINT16_MAX) + 1u) *
                                 AudioPacket::kAudioBytesPerPacket,
                             transfer.acceptedBytes());
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(UINT16_MAX) + 1u,
                             transfer.expectedSequence());
}

void test_receiver_nonzero_first_sequence_aborts()
{
    FakeStorage storage;
    Transfer transfer(&storage, fakeCallbacks());
    const uint8_t value = 1;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Started),
                          static_cast<int>(transfer.start(12, 0)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Aborted),
                          static_cast<int>(transfer.accept(
                              packet(1, 1, AudioPacket::kFirst), &value, 1)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Failure::MissingFirst),
                          static_cast<int>(transfer.failure()));
    TEST_ASSERT_EQUAL_UINT32(0, storage.write_calls);
}

void test_receiver_first_packet_without_first_flag_aborts()
{
    FakeStorage storage;
    Transfer transfer(&storage, fakeCallbacks());
    const uint8_t value = 1;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Started),
                          static_cast<int>(transfer.start(13, 0)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Aborted),
                          static_cast<int>(transfer.accept(packet(0, 1, 0), &value, 1)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Failure::MissingFirst),
                          static_cast<int>(transfer.failure()));
}

void test_receiver_one_packet_gap_aborts_without_writing_later_bytes()
{
    FakeStorage storage;
    Transfer transfer(&storage, fakeCallbacks());
    const uint8_t first = 1;
    const uint8_t later = 3;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Started),
                          static_cast<int>(transfer.start(14, 0)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Accepted),
                          static_cast<int>(transfer.accept(
                              packet(0, 1, AudioPacket::kFirst), &first, 1)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Aborted),
                          static_cast<int>(transfer.accept(packet(2, 1, 0), &later, 2)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Failure::SequenceGap),
                          static_cast<int>(transfer.failure()));
    TEST_ASSERT_EQUAL_UINT32(1, storage.write_calls);
    TEST_ASSERT_EQUAL_UINT32(0, storage.publish_calls);
}

void test_receiver_multiple_packet_gap_aborts()
{
    FakeStorage storage;
    Transfer transfer(&storage, fakeCallbacks());
    const uint8_t value = 1;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Started),
                          static_cast<int>(transfer.start(15, 0)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Accepted),
                          static_cast<int>(transfer.accept(
                              packet(0, 1, AudioPacket::kFirst), &value, 1)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Aborted),
                          static_cast<int>(transfer.accept(
                              packet(5, 1, AudioPacket::kLast), &value, 2)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Failure::SequenceGap),
                          static_cast<int>(transfer.failure()));
}

void test_receiver_duplicate_most_recent_packet_is_idempotent()
{
    FakeStorage storage;
    Transfer transfer(&storage, fakeCallbacks());
    const uint8_t value = 0x33;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Started),
                          static_cast<int>(transfer.start(16, 0)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Accepted),
                          static_cast<int>(transfer.accept(
                              packet(0, 1, AudioPacket::kFirst), &value, 1)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Duplicate),
                          static_cast<int>(transfer.accept(
                              packet(0, 1, AudioPacket::kFirst), &value, 2)));
    TEST_ASSERT_EQUAL_UINT32(1, storage.write_calls);
    TEST_ASSERT_EQUAL_UINT32(1, transfer.expectedSequence());
}

void test_receiver_duplicate_earlier_packet_never_writes_twice()
{
    FakeStorage storage;
    Transfer transfer(&storage, fakeCallbacks());
    const uint8_t values[] = {0x10, 0x20};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Started),
                          static_cast<int>(transfer.start(17, 0)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Accepted),
                          static_cast<int>(transfer.accept(
                              packet(0, 1, AudioPacket::kFirst), &values[0], 1)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Accepted),
                          static_cast<int>(transfer.accept(packet(1, 1, 0), &values[1], 2)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Duplicate),
                          static_cast<int>(transfer.accept(
                              packet(0, 1, AudioPacket::kFirst), &values[0], 3)));
    TEST_ASSERT_EQUAL_UINT32(2, storage.write_calls);
}

void test_receiver_first_flag_on_later_expected_packet_aborts()
{
    FakeStorage storage;
    Transfer transfer(&storage, fakeCallbacks());
    const uint8_t value = 1;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Started),
                          static_cast<int>(transfer.start(18, 0)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Accepted),
                          static_cast<int>(transfer.accept(
                              packet(0, 1, AudioPacket::kFirst), &value, 1)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Aborted),
                          static_cast<int>(transfer.accept(
                              packet(1, 1, AudioPacket::kFirst), &value, 2)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Failure::UnexpectedFirst),
                          static_cast<int>(transfer.failure()));
}

void test_receiver_data_without_start_is_rejected()
{
    FakeStorage storage;
    Transfer transfer(&storage, fakeCallbacks());
    const uint8_t value = 1;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::NoActiveSession),
                          static_cast<int>(transfer.accept(
                              packet(0, 1, AudioPacket::kFirst | AudioPacket::kLast),
                              &value,
                              1)));
    TEST_ASSERT_EQUAL_UINT32(0, storage.write_calls);
}

void test_receiver_sequence_65535_without_last_aborts_before_wrap()
{
    FakeStorage storage;
    storage.capture_bytes = false;
    Transfer transfer(&storage, fakeCallbacks());
    const uint8_t value = 1;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Started),
                          static_cast<int>(transfer.start(19, 0)));
    for (uint32_t sequence = 0; sequence < UINT16_MAX; ++sequence) {
        const uint8_t flags = sequence == 0 ? AudioPacket::kFirst : 0;
        TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Accepted),
                              static_cast<int>(transfer.accept(
                                  packet(static_cast<uint16_t>(sequence), 1, flags),
                                  &value,
                                  sequence + 1u)));
    }
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Aborted),
                          static_cast<int>(transfer.accept(
                              packet(UINT16_MAX, 1, 0), &value, UINT16_MAX + 1u)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Failure::SequenceLimit),
                          static_cast<int>(transfer.failure()));
    const uint32_t writes_before_wrap = storage.write_calls;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::NoActiveSession),
                          static_cast<int>(transfer.accept(
                              packet(0, 1, AudioPacket::kLast), &value, UINT16_MAX + 2u)));
    TEST_ASSERT_EQUAL_UINT32(writes_before_wrap, storage.write_calls);
}

void test_receiver_last_after_gap_never_publishes()
{
    FakeStorage storage;
    Transfer transfer(&storage, fakeCallbacks());
    const uint8_t value = 1;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Started),
                          static_cast<int>(transfer.start(20, 0)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Accepted),
                          static_cast<int>(transfer.accept(
                              packet(0, 1, AudioPacket::kFirst), &value, 1)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Aborted),
                          static_cast<int>(transfer.accept(
                              packet(2, 1, AudioPacket::kLast), &value, 2)));
    TEST_ASSERT_EQUAL_UINT32(0, storage.publish_calls);
}

void test_receiver_duplicate_last_after_completion_is_ignored()
{
    FakeStorage storage;
    Transfer transfer(&storage, fakeCallbacks());
    const uint8_t value = 1;
    const Packet only = packet(0, 1, AudioPacket::kFirst | AudioPacket::kLast);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Started),
                          static_cast<int>(transfer.start(21, 0)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Completed),
                          static_cast<int>(transfer.accept(only, &value, 1)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::AlreadyComplete),
                          static_cast<int>(transfer.accept(only, &value, 2)));
    TEST_ASSERT_EQUAL_UINT32(1, storage.write_calls);
    TEST_ASSERT_EQUAL_UINT32(1, storage.publish_calls);
}

void test_receiver_packet_after_completion_is_not_written()
{
    FakeStorage storage;
    Transfer transfer(&storage, fakeCallbacks());
    const uint8_t value = 1;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Started),
                          static_cast<int>(transfer.start(22, 0)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Completed),
                          static_cast<int>(transfer.accept(
                              packet(0, 1, AudioPacket::kFirst | AudioPacket::kLast),
                              &value,
                              1)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::AlreadyComplete),
                          static_cast<int>(transfer.accept(packet(1, 1, 0), &value, 2)));
    TEST_ASSERT_EQUAL_UINT32(1, storage.write_calls);
}

void test_receiver_new_start_is_rejected_while_active()
{
    FakeStorage storage;
    Transfer transfer(&storage, fakeCallbacks());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Started),
                          static_cast<int>(transfer.start(23, 0)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::StartRejectedActive),
                          static_cast<int>(transfer.start(24, 1)));
    TEST_ASSERT_EQUAL_UINT16(23, transfer.streamId());
    TEST_ASSERT_EQUAL_UINT32(1, storage.prepare_calls);
}

void test_receiver_stop_must_match_active_stream_and_cleans_partial()
{
    FakeStorage storage;
    Transfer transfer(&storage, fakeCallbacks());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Started),
                          static_cast<int>(transfer.start(25, 0)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::IgnoredStop),
                          static_cast<int>(transfer.stop(26)));
    TEST_ASSERT_TRUE(transfer.active());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Stopped),
                          static_cast<int>(transfer.stop(25)));
    TEST_ASSERT_FALSE(storage.partial_exists);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Failure::Cancelled),
                          static_cast<int>(transfer.failure()));
}

void test_receiver_timeout_with_zero_packets_aborts_and_cleans()
{
    FakeStorage storage;
    Transfer transfer(&storage, fakeCallbacks());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Started),
                          static_cast<int>(transfer.start(27, 100)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::TimedOut),
                          static_cast<int>(transfer.checkTimeout(10100)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Failure::Timeout),
                          static_cast<int>(transfer.failure()));
    TEST_ASSERT_FALSE(storage.partial_exists);
}

void test_receiver_timeout_after_data_never_publishes()
{
    FakeStorage storage;
    Transfer transfer(&storage, fakeCallbacks());
    const uint8_t value = 1;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Started),
                          static_cast<int>(transfer.start(28, 0)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Accepted),
                          static_cast<int>(transfer.accept(
                              packet(0, 1, AudioPacket::kFirst), &value, 500)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::TimedOut),
                          static_cast<int>(transfer.checkTimeout(10500)));
    TEST_ASSERT_EQUAL_UINT32(0, storage.publish_calls);
}

void test_receiver_timeout_resets_after_each_accepted_packet()
{
    FakeStorage storage;
    Transfer transfer(&storage, fakeCallbacks());
    const uint8_t value = 1;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Started),
                          static_cast<int>(transfer.start(29, 0)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Accepted),
                          static_cast<int>(transfer.accept(
                              packet(0, 1, AudioPacket::kFirst), &value, 9000)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Accepted),
                          static_cast<int>(transfer.checkTimeout(10000)));
    TEST_ASSERT_TRUE(transfer.active());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::TimedOut),
                          static_cast<int>(transfer.checkTimeout(19000)));
}

void test_receiver_duplicate_does_not_reset_timeout()
{
    FakeStorage storage;
    Transfer transfer(&storage, fakeCallbacks());
    const uint8_t value = 1;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Started),
                          static_cast<int>(transfer.start(30, 0)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Accepted),
                          static_cast<int>(transfer.accept(
                              packet(0, 1, AudioPacket::kFirst), &value, 100)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Duplicate),
                          static_cast<int>(transfer.accept(
                              packet(0, 1, AudioPacket::kFirst), &value, 9000)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::TimedOut),
                          static_cast<int>(transfer.checkTimeout(10100)));
}

void test_receiver_repeated_timeout_and_abort_cleanup_are_safe()
{
    FakeStorage storage;
    Transfer transfer(&storage, fakeCallbacks());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Started),
                          static_cast<int>(transfer.start(31, 0)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::TimedOut),
                          static_cast<int>(transfer.checkTimeout(10000)));
    const uint32_t removes = storage.remove_calls;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Aborted),
                          static_cast<int>(transfer.abort(Failure::Cancelled)));
    TEST_ASSERT_EQUAL_UINT32(removes, storage.remove_calls);
}

void test_receiver_new_transfer_can_start_after_timeout()
{
    FakeStorage storage;
    Transfer transfer(&storage, fakeCallbacks());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Started),
                          static_cast<int>(transfer.start(32, 0)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::TimedOut),
                          static_cast<int>(transfer.checkTimeout(10000)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Started),
                          static_cast<int>(transfer.start(33, 10001)));
    TEST_ASSERT_EQUAL_UINT16(33, transfer.streamId());
}

void test_receiver_new_transfer_can_start_after_sequence_gap()
{
    FakeStorage storage;
    Transfer transfer(&storage, fakeCallbacks());
    const uint8_t value = 1;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Started),
                          static_cast<int>(transfer.start(34, 0)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Accepted),
                          static_cast<int>(transfer.accept(
                              packet(0, 1, AudioPacket::kFirst), &value, 1)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Aborted),
                          static_cast<int>(transfer.accept(
                              packet(2, 1, 0), &value, 2)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Failure::SequenceGap),
                          static_cast<int>(transfer.failure()));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Started),
                          static_cast<int>(transfer.start(35, 3)));
}

void test_receiver_stale_partial_cleanup_failure_blocks_start()
{
    FakeStorage storage;
    storage.prepare_result = PrepareResult::CleanupFailed;
    Transfer transfer(&storage, fakeCallbacks());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Aborted),
                          static_cast<int>(transfer.start(36, 0)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Failure::CleanupFailed),
                          static_cast<int>(transfer.failure()));
    TEST_ASSERT_EQUAL_UINT32(0, storage.write_calls);
}

void test_receiver_open_failure_blocks_session()
{
    FakeStorage storage;
    storage.prepare_result = PrepareResult::OpenFailed;
    Transfer transfer(&storage, fakeCallbacks());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Aborted),
                          static_cast<int>(transfer.start(37, 0)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Failure::OpenFailed),
                          static_cast<int>(transfer.failure()));
}

void test_receiver_short_write_aborts_and_removes_partial()
{
    FakeStorage storage;
    storage.write_limit = 2;
    Transfer transfer(&storage, fakeCallbacks());
    const uint8_t values[] = {1, 2, 3};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Started),
                          static_cast<int>(transfer.start(38, 0)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Aborted),
                          static_cast<int>(transfer.accept(
                              packet(0, 3, AudioPacket::kFirst | AudioPacket::kLast),
                              values,
                              1)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Failure::WriteFailed),
                          static_cast<int>(transfer.failure()));
    TEST_ASSERT_FALSE(storage.partial_exists);
    TEST_ASSERT_EQUAL_UINT32(0, storage.publish_calls);
}

void test_receiver_write_failure_aborts()
{
    FakeStorage storage;
    storage.write_limit = 0;
    Transfer transfer(&storage, fakeCallbacks());
    const uint8_t value = 1;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Started),
                          static_cast<int>(transfer.start(39, 0)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Aborted),
                          static_cast<int>(transfer.accept(
                              packet(0, 1, AudioPacket::kFirst), &value, 1)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Failure::WriteFailed),
                          static_cast<int>(transfer.failure()));
}

void test_receiver_close_failure_prevents_publication()
{
    FakeStorage storage;
    storage.close_ok = false;
    Transfer transfer(&storage, fakeCallbacks());
    const uint8_t value = 1;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Started),
                          static_cast<int>(transfer.start(40, 0)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Aborted),
                          static_cast<int>(transfer.accept(
                              packet(0, 1, AudioPacket::kFirst | AudioPacket::kLast),
                              &value,
                              1)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Failure::CloseFailed),
                          static_cast<int>(transfer.failure()));
    TEST_ASSERT_EQUAL_UINT32(0, storage.publish_calls);
}

void test_receiver_publication_failure_removes_partial_and_preserves_existing()
{
    FakeStorage storage;
    storage.publish_ok = false;
    const std::vector<uint8_t> original = storage.existing_completed;
    Transfer transfer(&storage, fakeCallbacks());
    const uint8_t value = 1;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Started),
                          static_cast<int>(transfer.start(41, 0)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Aborted),
                          static_cast<int>(transfer.accept(
                              packet(0, 1, AudioPacket::kFirst | AudioPacket::kLast),
                              &value,
                              1)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Failure::PublicationFailed),
                          static_cast<int>(transfer.failure()));
    TEST_ASSERT_FALSE(storage.partial_exists);
    assertVectorEquals(original, storage.existing_completed);
}

void test_receiver_partial_remove_failure_is_reported_and_retryable()
{
    FakeStorage storage;
    storage.remove_ok = false;
    Transfer transfer(&storage, fakeCallbacks());
    const uint8_t value = 1;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Started),
                          static_cast<int>(transfer.start(42, 0)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Aborted),
                          static_cast<int>(transfer.accept(
                              packet(1, 1, AudioPacket::kFirst), &value, 1)));
    TEST_ASSERT_TRUE(transfer.cleanupFailed());
    TEST_ASSERT_TRUE(storage.partial_exists);

    storage.remove_ok = true;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Aborted),
                          static_cast<int>(transfer.abort(Failure::Cancelled)));
    TEST_ASSERT_FALSE(transfer.cleanupFailed());
    TEST_ASSERT_FALSE(storage.partial_exists);
}

void test_receiver_internal_partial_names_are_hidden_from_completed_listing()
{
    TEST_ASSERT_TRUE(FileReceiver::isInternalTransferName("rx_0001.part"));
    TEST_ASSERT_TRUE(FileReceiver::isInternalTransferName("anything.part"));
    TEST_ASSERT_FALSE(FileReceiver::isInternalTransferName("rx_0001.bin"));
    TEST_ASSERT_FALSE(FileReceiver::isInternalTransferName("song.u8"));
}

void test_receiver_startup_cleanup_selects_only_internal_partial_files()
{
    const std::vector<std::string> names{
        "rx_0001.part", "rx_0001.bin", "payload.bin", "orphan.part", "song.u8"};
    std::vector<std::string> removed;
    std::vector<std::string> retained;
    for (const std::string& name : names) {
        if (FileReceiver::isInternalTransferName(name)) {
            removed.push_back(name);
        } else {
            retained.push_back(name);
        }
    }
    TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(removed.size()));
    TEST_ASSERT_EQUAL_STRING("rx_0001.part", removed[0].c_str());
    TEST_ASSERT_EQUAL_STRING("orphan.part", removed[1].c_str());
    TEST_ASSERT_EQUAL_UINT32(3, static_cast<uint32_t>(retained.size()));
    TEST_ASSERT_EQUAL_STRING("rx_0001.bin", retained[0].c_str());
}

void test_receiver_zero_payload_length_is_invalid()
{
    FakeStorage storage;
    Transfer transfer(&storage, fakeCallbacks());
    const uint8_t value = 1;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Started),
                          static_cast<int>(transfer.start(43, 0)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Aborted),
                          static_cast<int>(transfer.accept(
                              packet(0, 0, AudioPacket::kFirst), &value, 1)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Failure::InvalidPacket),
                          static_cast<int>(transfer.failure()));
}

void test_receiver_unknown_flag_bits_are_invalid()
{
    FakeStorage storage;
    Transfer transfer(&storage, fakeCallbacks());
    const uint8_t value = 1;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Started),
                          static_cast<int>(transfer.start(44, 0)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Aborted),
                          static_cast<int>(transfer.accept(
                              packet(0, 1, AudioPacket::kFirst | 0x80), &value, 1)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Failure::InvalidPacket),
                          static_cast<int>(transfer.failure()));
}

void test_receiver_malformed_packet_aborts_active_session()
{
    FakeStorage storage;
    Transfer transfer(&storage, fakeCallbacks());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Started),
                          static_cast<int>(transfer.start(45, 0)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Aborted),
                          static_cast<int>(transfer.rejectMalformedPacket()));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Failure::InvalidPacket),
                          static_cast<int>(transfer.failure()));
    TEST_ASSERT_FALSE(storage.partial_exists);
}

void test_receiver_missing_storage_callbacks_fail_safely()
{
    Transfer transfer(nullptr, StorageCallbacks{});
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Aborted),
                          static_cast<int>(transfer.start(46, 0)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Failure::StateViolation),
                          static_cast<int>(transfer.failure()));
}

}  // namespace

void runReceiverSafetyTests()
{
    RUN_TEST(test_receiver_one_packet_transfer_publishes_exact_bytes);
    RUN_TEST(test_receiver_multi_packet_transfer_is_contiguous_and_binary_safe);
    RUN_TEST(test_receiver_exact_28_byte_packet_completes);
    RUN_TEST(test_receiver_all_byte_values_round_trip);
    RUN_TEST(test_receiver_maximum_sequence_completes_without_large_allocation);
    RUN_TEST(test_receiver_nonzero_first_sequence_aborts);
    RUN_TEST(test_receiver_first_packet_without_first_flag_aborts);
    RUN_TEST(test_receiver_one_packet_gap_aborts_without_writing_later_bytes);
    RUN_TEST(test_receiver_multiple_packet_gap_aborts);
    RUN_TEST(test_receiver_duplicate_most_recent_packet_is_idempotent);
    RUN_TEST(test_receiver_duplicate_earlier_packet_never_writes_twice);
    RUN_TEST(test_receiver_first_flag_on_later_expected_packet_aborts);
    RUN_TEST(test_receiver_data_without_start_is_rejected);
    RUN_TEST(test_receiver_sequence_65535_without_last_aborts_before_wrap);
    RUN_TEST(test_receiver_last_after_gap_never_publishes);
    RUN_TEST(test_receiver_duplicate_last_after_completion_is_ignored);
    RUN_TEST(test_receiver_packet_after_completion_is_not_written);
    RUN_TEST(test_receiver_new_start_is_rejected_while_active);
    RUN_TEST(test_receiver_stop_must_match_active_stream_and_cleans_partial);
    RUN_TEST(test_receiver_timeout_with_zero_packets_aborts_and_cleans);
    RUN_TEST(test_receiver_timeout_after_data_never_publishes);
    RUN_TEST(test_receiver_timeout_resets_after_each_accepted_packet);
    RUN_TEST(test_receiver_duplicate_does_not_reset_timeout);
    RUN_TEST(test_receiver_repeated_timeout_and_abort_cleanup_are_safe);
    RUN_TEST(test_receiver_new_transfer_can_start_after_timeout);
    RUN_TEST(test_receiver_new_transfer_can_start_after_sequence_gap);
    RUN_TEST(test_receiver_stale_partial_cleanup_failure_blocks_start);
    RUN_TEST(test_receiver_open_failure_blocks_session);
    RUN_TEST(test_receiver_short_write_aborts_and_removes_partial);
    RUN_TEST(test_receiver_write_failure_aborts);
    RUN_TEST(test_receiver_close_failure_prevents_publication);
    RUN_TEST(test_receiver_publication_failure_removes_partial_and_preserves_existing);
    RUN_TEST(test_receiver_partial_remove_failure_is_reported_and_retryable);
    RUN_TEST(test_receiver_internal_partial_names_are_hidden_from_completed_listing);
    RUN_TEST(test_receiver_startup_cleanup_selects_only_internal_partial_files);
    RUN_TEST(test_receiver_zero_payload_length_is_invalid);
    RUN_TEST(test_receiver_unknown_flag_bits_are_invalid);
    RUN_TEST(test_receiver_malformed_packet_aborts_active_session);
    RUN_TEST(test_receiver_missing_storage_callbacks_fail_safely);
}
