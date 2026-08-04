#include <unity.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "protocol_v2.hpp"
#include "../include/protocol_v2_fake_transport.hpp"
#include "reliable_transfer_v2.hpp"

namespace {

using ProtocolV2::DecodeStatus;
using ProtocolV2::ErrorCode;
using ProtocolV2::Frame;
using ProtocolV2::Packet;
using ProtocolV2::PacketType;
using ReliableTransferV2::InspectResult;
using ReliableTransferV2::Metadata;
using ReliableTransferV2::ReadResult;
using ReliableTransferV2::ReadState;
using ReliableTransferV2::ReceiverEvent;
using ReliableTransferV2::ReceiverSession;
using ReliableTransferV2::ReceiverState;
using ReliableTransferV2::SenderEvent;
using ReliableTransferV2::SenderSession;
using ReliableTransferV2::SenderState;
using ProtocolV2Test::Destination;
using ProtocolV2Test::FakeDuplexTransport;
using ProtocolV2Test::FaultKind;
using ProtocolV2Test::FaultRule;

uint8_t patternByte(uint64_t index)
{
    return static_cast<uint8_t>((index * 37u + 11u) & 0xFFu);
}

struct FakeSource {
    std::vector<uint8_t> bytes;
    uint64_t generated_size = 0;
    uint64_t position = 0;
    uint64_t fail_at = UINT64_MAX;
    size_t maximum_chunk = SIZE_MAX;
    bool generated = false;
    bool reset_ok = true;
    uint32_t resets = 0;
    uint64_t bytes_read = 0;

    uint64_t size() const { return generated ? generated_size : bytes.size(); }

    uint8_t at(uint64_t index) const
    {
        return generated ? patternByte(index) : bytes[static_cast<size_t>(index)];
    }
};

bool resetSource(void* context)
{
    FakeSource& source = *static_cast<FakeSource*>(context);
    ++source.resets;
    if (!source.reset_ok) {
        return false;
    }
    source.position = 0;
    return true;
}

ReadResult readSource(void* context, uint8_t* out, size_t capacity)
{
    FakeSource& source = *static_cast<FakeSource*>(context);
    if (!out || capacity == 0 || source.position >= source.fail_at) {
        return {0, ReadState::Error};
    }
    const uint64_t remaining = source.size() - source.position;
    if (remaining == 0) {
        return {0, ReadState::EndOfFile};
    }
    const uint64_t before_failure = source.fail_at - source.position;
    const size_t count = static_cast<size_t>(std::min<uint64_t>(
        std::min(remaining, before_failure),
        std::min(capacity, source.maximum_chunk)));
    for (size_t index = 0; index < count; ++index) {
        out[index] = source.at(source.position + index);
    }
    source.position += count;
    source.bytes_read += count;
    return {count, source.position == source.size()
                       ? ReadState::EndOfFile : ReadState::MoreDataMayFollow};
}

ReliableTransferV2::SourceCallbacks sourceCallbacks()
{
    return {&resetSource, &readSource};
}

struct FakeSink {
    std::vector<uint8_t> bytes;
    uint64_t received_size = 0;
    uint32_t transfer_id = 0;
    uint32_t declared_size = 0;
    uint32_t prepare_count = 0;
    uint32_t write_count = 0;
    uint32_t close_count = 0;
    uint32_t publish_count = 0;
    uint32_t remove_count = 0;
    bool collect = true;
    bool validate_pattern = false;
    bool pattern_valid = true;
    bool partial_exists = false;
    bool open = false;
    bool published = false;
    bool existing_completed_intact = true;
    ReliableTransferV2::SinkPrepareResult prepare_result =
        ReliableTransferV2::SinkPrepareResult::Ready;
    bool short_write = false;
    bool close_ok = true;
    bool publish_ok = true;
    bool remove_ok = true;
};

ReliableTransferV2::SinkPrepareResult prepareSink(void* context,
                                                   uint32_t transfer_id,
                                                   uint32_t total_size)
{
    FakeSink& sink = *static_cast<FakeSink*>(context);
    ++sink.prepare_count;
    sink.transfer_id = transfer_id;
    sink.declared_size = total_size;
    if (sink.prepare_result != ReliableTransferV2::SinkPrepareResult::Ready) {
        return sink.prepare_result;
    }
    sink.bytes.clear();
    sink.received_size = 0;
    sink.pattern_valid = true;
    sink.partial_exists = true;
    sink.open = true;
    sink.published = false;
    return ReliableTransferV2::SinkPrepareResult::Ready;
}

size_t writeSink(void* context, const uint8_t* data, size_t length)
{
    FakeSink& sink = *static_cast<FakeSink*>(context);
    ++sink.write_count;
    if (!sink.open || !data || length == 0) {
        return 0;
    }
    const size_t accepted = sink.short_write ? length - 1u : length;
    for (size_t index = 0; index < accepted; ++index) {
        if (sink.validate_pattern &&
            data[index] != patternByte(sink.received_size + index)) {
            sink.pattern_valid = false;
        }
        if (sink.collect) {
            sink.bytes.push_back(data[index]);
        }
    }
    sink.received_size += accepted;
    return accepted;
}

bool closeSink(void* context)
{
    FakeSink& sink = *static_cast<FakeSink*>(context);
    ++sink.close_count;
    sink.open = false;
    return sink.close_ok;
}

bool publishSink(void* context)
{
    FakeSink& sink = *static_cast<FakeSink*>(context);
    ++sink.publish_count;
    if (!sink.publish_ok || sink.open || !sink.partial_exists) {
        return false;
    }
    sink.partial_exists = false;
    sink.published = true;
    return true;
}

bool removeSink(void* context)
{
    FakeSink& sink = *static_cast<FakeSink*>(context);
    ++sink.remove_count;
    if (!sink.remove_ok) {
        return false;
    }
    sink.open = false;
    sink.partial_exists = false;
    return true;
}

ReliableTransferV2::SinkCallbacks sinkCallbacks()
{
    return {&prepareSink, &writeSink, &closeSink, &publishSink, &removeSink};
}

Frame encoded(const Packet& packet)
{
    Frame frame{};
    TEST_ASSERT_TRUE(ProtocolV2::encode(packet, frame));
    return frame;
}

Packet decoded(const Frame& frame)
{
    Packet packet{};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DecodeStatus::Ok),
                          static_cast<int>(ProtocolV2::decode(
                              frame.data(), frame.size(), packet)));
    return packet;
}

Packet startPacket(uint32_t id, const Metadata& metadata)
{
    Packet packet{};
    packet.type = PacketType::Start;
    packet.transfer_id = id;
    packet.total_size = metadata.total_size;
    packet.total_packets = metadata.total_packets;
    packet.crc32 = metadata.crc32;
    return packet;
}

Packet dataPacket(uint32_t id,
                  uint16_t sequence,
                  const uint8_t* data,
                  size_t length)
{
    Packet packet{};
    packet.type = PacketType::Data;
    packet.transfer_id = id;
    packet.sequence = sequence;
    packet.payload_length = static_cast<uint8_t>(length);
    for (size_t index = 0; index < length; ++index) {
        packet.payload[index] = data[index];
    }
    return packet;
}

Packet endPacket(uint32_t id, const Metadata& metadata)
{
    Packet packet = startPacket(id, metadata);
    packet.type = PacketType::End;
    return packet;
}

Metadata inspect(FakeSource& source)
{
    Metadata metadata{};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(InspectResult::Ready),
                          static_cast<int>(ReliableTransferV2::inspectSource(
                              &source, sourceCallbacks(), metadata)));
    return metadata;
}

struct Harness {
    FakeSource& source;
    FakeSink& sink;
    SenderSession sender;
    ReceiverSession receiver;
    FakeDuplexTransport transport;
    Metadata metadata{};
    uint64_t now_ms = 0;
    uint32_t transfer_id = 0x10203040u;

    Harness(FakeSource& source_ref, FakeSink& sink_ref, uint32_t seed = 1)
        : source(source_ref),
          sink(sink_ref),
          sender(&source, sourceCallbacks()),
          receiver(&sink, sinkCallbacks()),
          transport(seed)
    {
    }

    bool begin()
    {
        metadata = inspect(source);
        return sender.begin(transfer_id, metadata, now_ms);
    }

    void step()
    {
        Frame frame{};
        if (sender.outboundFrame(frame)) {
            TEST_ASSERT_TRUE(transport.send(Destination::Receiver, frame, now_ms));
            TEST_ASSERT_TRUE(sender.noteFrameSent(now_ms));
        }

        while (transport.pop(Destination::Receiver, now_ms, frame)) {
            Frame response{};
            const ReceiverEvent event = receiver.onFrame(
                frame.data(), frame.size(), now_ms, response);
            if (event != ReceiverEvent::Ignored) {
                TEST_ASSERT_TRUE(transport.send(Destination::Sender, response, now_ms));
            }
        }

        while (transport.pop(Destination::Sender, now_ms, frame)) {
            (void)sender.onFrame(frame.data(), frame.size(), now_ms);
        }

        Frame timeout_response{};
        const ReceiverEvent timeout_event = receiver.tick(now_ms, timeout_response);
        if (timeout_event == ReceiverEvent::TimedOut) {
            TEST_ASSERT_TRUE(transport.send(
                Destination::Sender, timeout_response, now_ms));
        }
        (void)sender.tick(now_ms);
        now_ms += 10;
    }

    bool run(size_t maximum_steps = 300000)
    {
        for (size_t step_index = 0;
             step_index < maximum_steps && !sender.terminal();
             ++step_index) {
            step();
        }
        return sender.state() == SenderState::Completed;
    }
};

void assertSuccessfulTransfer(Harness& harness)
{
    TEST_ASSERT_TRUE(harness.run());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SenderState::Completed),
                          static_cast<int>(harness.sender.state()));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ReceiverState::Completed),
                          static_cast<int>(harness.receiver.state()));
    TEST_ASSERT_TRUE(harness.sender.peerComplete());
    TEST_ASSERT_TRUE(harness.sink.published);
    TEST_ASSERT_FALSE(harness.sink.partial_exists);
    TEST_ASSERT_EQUAL_UINT32(harness.metadata.total_size,
                             harness.sender.acknowledgedBytes());
    TEST_ASSERT_EQUAL_UINT32(harness.metadata.total_packets,
                             harness.sender.acknowledgedPackets());
}

void test_protocol_v2_constants_and_size_limits()
{
    TEST_ASSERT_EQUAL_UINT32(32, ProtocolV2::kFrameSize);
    TEST_ASSERT_EQUAL_UINT32(20, ProtocolV2::kDataPayloadCapacity);
    TEST_ASSERT_EQUAL_UINT32(65536, ProtocolV2::kMaxPacketCount);
    TEST_ASSERT_EQUAL_UINT32(1310720, ProtocolV2::kMaxFileSize);
    TEST_ASSERT_EQUAL_UINT32(0, ProtocolV2::packetCountForSize(0));
    TEST_ASSERT_EQUAL_UINT32(1, ProtocolV2::packetCountForSize(20));
    TEST_ASSERT_EQUAL_UINT32(2, ProtocolV2::packetCountForSize(21));
}

void test_protocol_v2_start_round_trip_and_little_endian_layout()
{
    Packet packet{};
    packet.type = PacketType::Start;
    packet.transfer_id = 0x78563412u;
    packet.total_size = 21;
    packet.total_packets = 2;
    packet.crc32 = 0xA1B2C3D4u;
    const Frame frame = encoded(packet);
    TEST_ASSERT_EQUAL_HEX8('R', frame[0]);
    TEST_ASSERT_EQUAL_HEX8('F', frame[1]);
    TEST_ASSERT_EQUAL_HEX8('2', frame[2]);
    TEST_ASSERT_EQUAL_HEX8(2, frame[3]);
    TEST_ASSERT_EQUAL_HEX8(0x12, frame[5]);
    TEST_ASSERT_EQUAL_HEX8(0x34, frame[6]);
    TEST_ASSERT_EQUAL_HEX8(0x56, frame[7]);
    TEST_ASSERT_EQUAL_HEX8(0x78, frame[8]);
    TEST_ASSERT_EQUAL_HEX8(21, frame[9]);
    const Packet result = decoded(frame);
    TEST_ASSERT_EQUAL_UINT32(packet.transfer_id, result.transfer_id);
    TEST_ASSERT_EQUAL_UINT32(21, result.total_size);
    TEST_ASSERT_EQUAL_UINT32(2, result.total_packets);
    TEST_ASSERT_EQUAL_HEX32(packet.crc32, result.crc32);
}

void test_protocol_v2_every_control_packet_round_trips()
{
    const PacketType types[] = {PacketType::Ready, PacketType::Ack,
                                PacketType::Nack, PacketType::End,
                                PacketType::Complete, PacketType::Error,
                                PacketType::Cancel};
    for (PacketType type : types) {
        Packet packet{};
        packet.type = type;
        packet.transfer_id = 9;
        packet.total_size = 40;
        packet.total_packets = 2;
        packet.crc32 = 0x12345678u;
        packet.sequence = 1;
        packet.expected_sequence = 2;
        packet.accepted = true;
        packet.error = ErrorCode::None;
        packet.relevant_sequence = 7;
        if (type == PacketType::Nack || type == PacketType::Error ||
            type == PacketType::Cancel) {
            packet.error = ErrorCode::UnexpectedSequence;
        }
        const Packet result = decoded(encoded(packet));
        TEST_ASSERT_EQUAL_INT(static_cast<int>(type), static_cast<int>(result.type));
        TEST_ASSERT_EQUAL_UINT32(9, result.transfer_id);
    }
}

void test_protocol_v2_data_round_trip_preserves_binary_and_zero_padding()
{
    std::array<uint8_t, ProtocolV2::kDataPayloadCapacity> bytes{};
    for (size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<uint8_t>(index == 0 ? 0x00 :
                                           index == 1 ? 0xFF : index);
    }
    Packet packet = dataPacket(7, 65535, bytes.data(), 17);
    const Frame frame = encoded(packet);
    for (size_t index = 29; index < frame.size(); ++index) {
        TEST_ASSERT_EQUAL_HEX8(0, frame[index]);
    }
    const Packet result = decoded(frame);
    TEST_ASSERT_EQUAL_UINT16(65535, result.sequence);
    TEST_ASSERT_EQUAL_UINT8(17, result.payload_length);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(bytes.data(), result.payload.data(), 17);
}

void test_protocol_v2_decode_rejects_magic_version_type_and_length_errors()
{
    Packet packet = dataPacket(1, 0, reinterpret_cast<const uint8_t*>("a"), 1);
    Frame frame = encoded(packet);
    Packet out{};
    frame[0] ^= 1;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DecodeStatus::InvalidMagic),
                          static_cast<int>(ProtocolV2::decode(frame.data(), 32, out)));
    frame = encoded(packet);
    frame[3] = 99;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DecodeStatus::UnsupportedVersion),
                          static_cast<int>(ProtocolV2::decode(frame.data(), 32, out)));
    frame = encoded(packet);
    frame[4] = 99;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DecodeStatus::UnknownPacketType),
                          static_cast<int>(ProtocolV2::decode(frame.data(), 32, out)));
    frame = encoded(packet);
    frame[11] = 21;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DecodeStatus::InvalidField),
                          static_cast<int>(ProtocolV2::decode(frame.data(), 32, out)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DecodeStatus::WrongSize),
                          static_cast<int>(ProtocolV2::decode(frame.data(), 31, out)));
}

void test_protocol_v2_decode_rejects_nonzero_reserved_and_padding_bytes()
{
    Metadata metadata{1, 1, 0};
    Frame start = encoded(startPacket(2, metadata));
    start[31] = 1;
    Packet out{};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DecodeStatus::NonzeroReserved),
                          static_cast<int>(ProtocolV2::decode(start.data(), 32, out)));
    const uint8_t value = 9;
    Frame data = encoded(dataPacket(2, 0, &value, 1));
    data[31] = 1;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DecodeStatus::NonzeroReserved),
                          static_cast<int>(ProtocolV2::decode(data.data(), 32, out)));
}

void test_protocol_v2_rejects_unknown_error_codes()
{
    Packet error{};
    error.type = PacketType::Error;
    error.transfer_id = 3;
    error.error = ErrorCode::Timeout;
    Frame frame = encoded(error);
    Packet out{};
    frame[9] = 0xFF;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DecodeStatus::InvalidField),
                          static_cast<int>(ProtocolV2::decode(
                              frame.data(), frame.size(), out)));

    Packet ready{};
    ready.type = PacketType::Ready;
    ready.transfer_id = 3;
    ready.accepted = false;
    ready.error = ErrorCode::Busy;
    frame = encoded(ready);
    frame[14] = 0xFF;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DecodeStatus::InvalidField),
                          static_cast<int>(ProtocolV2::decode(
                              frame.data(), frame.size(), out)));

    error.error = static_cast<ErrorCode>(0xFF);
    TEST_ASSERT_FALSE(ProtocolV2::encode(error, frame));
}

void test_protocol_v2_encode_rejects_zero_id_invalid_metadata_and_zero_data()
{
    Packet start = startPacket(0, Metadata{1, 1, 0});
    Frame frame{};
    TEST_ASSERT_FALSE(ProtocolV2::encode(start, frame));
    start.transfer_id = 1;
    start.total_packets = 2;
    TEST_ASSERT_FALSE(ProtocolV2::encode(start, frame));
    Packet data{};
    data.type = PacketType::Data;
    data.transfer_id = 1;
    TEST_ASSERT_FALSE(ProtocolV2::encode(data, frame));
}

void test_protocol_v2_crc32_known_vectors()
{
    TEST_ASSERT_EQUAL_HEX32(0x00000000u, ProtocolV2::calculateCrc32(nullptr, 0));
    const uint8_t digits[] = {'1','2','3','4','5','6','7','8','9'};
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u,
                            ProtocolV2::calculateCrc32(digits, sizeof(digits)));
    std::array<uint8_t, 256> all{};
    for (size_t index = 0; index < all.size(); ++index) {
        all[index] = static_cast<uint8_t>(index);
    }
    TEST_ASSERT_EQUAL_HEX32(0x29058C73u,
                            ProtocolV2::calculateCrc32(all.data(), all.size()));
}

void test_protocol_v2_crc32_incremental_matches_one_pass()
{
    std::array<uint8_t, 257> bytes{};
    for (size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = patternByte(index);
    }
    ProtocolV2::Crc32 crc;
    crc.update(bytes.data(), 3);
    crc.update(bytes.data() + 3, 129);
    crc.update(bytes.data() + 132, bytes.size() - 132);
    TEST_ASSERT_EQUAL_HEX32(ProtocolV2::calculateCrc32(bytes.data(), bytes.size()),
                            crc.value());
}

void test_protocol_v2_source_inspection_covers_zero_boundaries_and_partial_reads()
{
    const size_t sizes[] = {0, 1, 20, 21, 40};
    for (size_t size : sizes) {
        FakeSource source;
        source.generated = true;
        source.generated_size = size;
        source.maximum_chunk = 3;
        const Metadata metadata = inspect(source);
        TEST_ASSERT_EQUAL_UINT32(size, metadata.total_size);
        TEST_ASSERT_EQUAL_UINT32(ProtocolV2::packetCountForSize(size),
                                 metadata.total_packets);
        TEST_ASSERT_EQUAL_UINT64(0, source.position);
    }
}

void test_protocol_v2_source_inspection_maximum_is_incremental()
{
    FakeSource source;
    source.generated = true;
    source.generated_size = ProtocolV2::kMaxFileSize;
    source.maximum_chunk = 97;
    const Metadata metadata = inspect(source);
    TEST_ASSERT_EQUAL_UINT32(ProtocolV2::kMaxFileSize, metadata.total_size);
    TEST_ASSERT_EQUAL_UINT32(ProtocolV2::kMaxPacketCount, metadata.total_packets);
    TEST_ASSERT_TRUE(source.bytes.empty());
    TEST_ASSERT_EQUAL_UINT64(0, source.position);
}

void test_protocol_v2_source_inspection_rejects_over_limit_without_full_buffer()
{
    FakeSource source;
    source.generated = true;
    source.generated_size = static_cast<uint64_t>(ProtocolV2::kMaxFileSize) + 1u;
    Metadata metadata{};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(InspectResult::UnsupportedSize),
                          static_cast<int>(ReliableTransferV2::inspectSource(
                              &source, sourceCallbacks(), metadata)));
    TEST_ASSERT_TRUE(source.bytes.empty());
    TEST_ASSERT_TRUE(source.bytes_read <=
                     static_cast<uint64_t>(ProtocolV2::kMaxFileSize) + 128u);
    TEST_ASSERT_EQUAL_UINT64(0, source.position);
}

void test_protocol_v2_source_inspection_propagates_read_and_reset_errors()
{
    FakeSource source;
    source.generated = true;
    source.generated_size = 100;
    source.fail_at = 30;
    Metadata metadata{};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(InspectResult::ReadFailed),
                          static_cast<int>(ReliableTransferV2::inspectSource(
                              &source, sourceCallbacks(), metadata)));
    source = FakeSource{};
    source.reset_ok = false;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(InspectResult::ResetFailed),
                          static_cast<int>(ReliableTransferV2::inspectSource(
                              &source, sourceCallbacks(), metadata)));
}

void test_protocol_v2_receiver_start_ready_duplicate_and_busy_behavior()
{
    FakeSink sink;
    ReceiverSession receiver(&sink, sinkCallbacks());
    FakeSource source;
    source.bytes = {1, 2, 3};
    const Metadata metadata = inspect(source);
    Frame response{};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ReceiverEvent::ResponseReady),
                          static_cast<int>(receiver.onPacket(
                              startPacket(10, metadata), 0, response)));
    TEST_ASSERT_TRUE(decoded(response).accepted);
    TEST_ASSERT_EQUAL_UINT32(1, sink.prepare_count);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ReceiverEvent::ResponseReady),
                          static_cast<int>(receiver.onPacket(
                              startPacket(10, metadata), 1, response)));
    TEST_ASSERT_EQUAL_UINT32(1, sink.prepare_count);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ReceiverEvent::ResponseReady),
                          static_cast<int>(receiver.onPacket(
                              startPacket(11, metadata), 2, response)));
    const Packet busy = decoded(response);
    TEST_ASSERT_FALSE(busy.accepted);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ErrorCode::Busy),
                          static_cast<int>(busy.error));
}

void test_protocol_v2_receiver_data_gap_duplicate_and_wrong_id_are_safe()
{
    FakeSink sink;
    ReceiverSession receiver(&sink, sinkCallbacks());
    FakeSource source;
    source.generated = true;
    source.generated_size = 21;
    const Metadata metadata = inspect(source);
    Frame response{};
    (void)receiver.onPacket(startPacket(7, metadata), 0, response);
    uint8_t bytes[20] = {};
    for (size_t i = 0; i < 20; ++i) bytes[i] = patternByte(i);
    Packet future = dataPacket(7, 1, bytes, 1);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ReceiverEvent::ResponseReady),
                          static_cast<int>(receiver.onPacket(future, 1, response)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(PacketType::Nack),
                          static_cast<int>(decoded(response).type));
    TEST_ASSERT_EQUAL_UINT32(0, sink.write_count);
    Packet wrong = dataPacket(8, 0, bytes, 20);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ReceiverEvent::Ignored),
                          static_cast<int>(receiver.onPacket(wrong, 2, response)));
    Packet first = dataPacket(7, 0, bytes, 20);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ReceiverEvent::DataAccepted),
                          static_cast<int>(receiver.onPacket(first, 3, response)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ReceiverEvent::Duplicate),
                          static_cast<int>(receiver.onPacket(first, 4, response)));
    TEST_ASSERT_EQUAL_UINT32(1, sink.write_count);
    TEST_ASSERT_EQUAL_UINT32(20, sink.received_size);
}

void test_protocol_v2_receiver_valid_end_publishes_and_duplicate_end_replies_complete()
{
    FakeSink sink;
    ReceiverSession receiver(&sink, sinkCallbacks());
    FakeSource source;
    source.bytes = {0, 0xFF, 4};
    const Metadata metadata = inspect(source);
    Frame response{};
    (void)receiver.onPacket(startPacket(15, metadata), 0, response);
    const Packet data = dataPacket(15, 0, source.bytes.data(), source.bytes.size());
    (void)receiver.onPacket(data, 1, response);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ReceiverEvent::Completed),
                          static_cast<int>(receiver.onPacket(
                              endPacket(15, metadata), 2, response)));
    TEST_ASSERT_TRUE(sink.published);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(source.bytes.data(), sink.bytes.data(), source.bytes.size());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(PacketType::Complete),
                          static_cast<int>(decoded(response).type));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ReceiverEvent::Completed),
                          static_cast<int>(receiver.onPacket(
                              endPacket(15, metadata), 3, response)));
    TEST_ASSERT_EQUAL_UINT32(1, sink.publish_count);
}

void test_protocol_v2_receiver_crc_mismatch_and_extra_data_never_publish()
{
    FakeSink sink;
    ReceiverSession receiver(&sink, sinkCallbacks());
    FakeSource source;
    source.bytes = {1, 2, 3};
    const Metadata metadata = inspect(source);
    Frame response{};
    (void)receiver.onPacket(startPacket(20, metadata), 0, response);
    uint8_t corrupt[] = {1, 9, 3};
    (void)receiver.onPacket(dataPacket(20, 0, corrupt, 3), 1, response);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ReceiverEvent::Failed),
                          static_cast<int>(receiver.onPacket(
                              endPacket(20, metadata), 2, response)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ErrorCode::CrcMismatch),
                          static_cast<int>(receiver.error()));
    TEST_ASSERT_FALSE(sink.published);
    TEST_ASSERT_FALSE(sink.partial_exists);

    FakeSink second_sink;
    ReceiverSession second(&second_sink, sinkCallbacks());
    (void)second.onPacket(startPacket(21, metadata), 0, response);
    (void)second.onPacket(dataPacket(21, 0, source.bytes.data(), 3), 1, response);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ReceiverEvent::Failed),
                          static_cast<int>(second.onPacket(
                              dataPacket(21, 1, source.bytes.data(), 1), 2, response)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ErrorCode::PacketCountMismatch),
                          static_cast<int>(second.error()));
    TEST_ASSERT_FALSE(second_sink.published);
}

void test_protocol_v2_receiver_cancel_timeout_and_new_transfer_cleanup()
{
    FakeSink sink;
    ReceiverSession receiver(&sink, sinkCallbacks(), 100);
    FakeSource source;
    source.bytes = {1};
    const Metadata metadata = inspect(source);
    Frame response{};
    (void)receiver.onPacket(startPacket(30, metadata), 0, response);
    Packet cancel{};
    cancel.type = PacketType::Cancel;
    cancel.error = ErrorCode::Cancelled;
    cancel.transfer_id = 31;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ReceiverEvent::Ignored),
                          static_cast<int>(receiver.onPacket(cancel, 1, response)));
    TEST_ASSERT_TRUE(receiver.active());
    TEST_ASSERT_TRUE(sink.partial_exists);
    cancel.transfer_id = 30;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ReceiverEvent::Cancelled),
                          static_cast<int>(receiver.onPacket(cancel, 2, response)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ReceiverEvent::Cancelled),
                          static_cast<int>(receiver.onPacket(cancel, 3, response)));
    TEST_ASSERT_FALSE(sink.partial_exists);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ReceiverEvent::ResponseReady),
                          static_cast<int>(receiver.onPacket(
                              startPacket(31, metadata), 4, response)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ReceiverEvent::TimedOut),
                          static_cast<int>(receiver.tick(104, response)));
    TEST_ASSERT_FALSE(sink.partial_exists);
    TEST_ASSERT_FALSE(sink.published);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ReceiverEvent::ResponseReady),
                          static_cast<int>(receiver.onPacket(
                              startPacket(32, metadata), 104, response)));
}

void test_protocol_v2_receiver_zero_byte_file_is_verified_and_published()
{
    FakeSink sink;
    ReceiverSession receiver(&sink, sinkCallbacks());
    FakeSource source;
    const Metadata metadata = inspect(source);
    Frame response{};
    (void)receiver.onPacket(startPacket(40, metadata), 0, response);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ReceiverState::WaitingForEnd),
                          static_cast<int>(receiver.state()));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ReceiverEvent::Completed),
                          static_cast<int>(receiver.onPacket(
                              endPacket(40, metadata), 1, response)));
    TEST_ASSERT_TRUE(sink.published);
    TEST_ASSERT_EQUAL_UINT32(0, sink.received_size);
}

void test_protocol_v2_receiver_storage_failures_preserve_existing_completed_file()
{
    FakeSource source;
    source.bytes = {1};
    const Metadata metadata = inspect(source);
    Frame response{};

    FakeSink open_fail;
    open_fail.prepare_result = ReliableTransferV2::SinkPrepareResult::OpenFailed;
    ReceiverSession first(&open_fail, sinkCallbacks());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ReceiverEvent::Failed),
                          static_cast<int>(first.onPacket(
                              startPacket(50, metadata), 0, response)));
    TEST_ASSERT_TRUE(open_fail.existing_completed_intact);

    FakeSink write_fail;
    write_fail.short_write = true;
    ReceiverSession second(&write_fail, sinkCallbacks());
    (void)second.onPacket(startPacket(51, metadata), 0, response);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ReceiverEvent::Failed),
                          static_cast<int>(second.onPacket(
                              dataPacket(51, 0, source.bytes.data(), 1), 1, response)));
    TEST_ASSERT_FALSE(write_fail.published);
    TEST_ASSERT_TRUE(write_fail.existing_completed_intact);

    FakeSink close_fail;
    close_fail.close_ok = false;
    ReceiverSession third(&close_fail, sinkCallbacks());
    (void)third.onPacket(startPacket(52, metadata), 0, response);
    (void)third.onPacket(dataPacket(52, 0, source.bytes.data(), 1), 1, response);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ReceiverEvent::Failed),
                          static_cast<int>(third.onPacket(
                              endPacket(52, metadata), 2, response)));
    TEST_ASSERT_FALSE(close_fail.published);

    FakeSink publish_fail;
    publish_fail.publish_ok = false;
    ReceiverSession fourth(&publish_fail, sinkCallbacks());
    (void)fourth.onPacket(startPacket(53, metadata), 0, response);
    (void)fourth.onPacket(dataPacket(53, 0, source.bytes.data(), 1), 1, response);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ReceiverEvent::Failed),
                          static_cast<int>(fourth.onPacket(
                              endPacket(53, metadata), 2, response)));
    TEST_ASSERT_FALSE(publish_fail.published);
    TEST_ASSERT_TRUE(publish_fail.existing_completed_intact);
}

void test_protocol_v2_receiver_cleanup_failure_is_reported_and_retryable()
{
    FakeSource source;
    source.bytes = {1};
    const Metadata metadata = inspect(source);
    FakeSink sink;
    sink.remove_ok = false;
    ReceiverSession receiver(&sink, sinkCallbacks());
    Frame response{};
    (void)receiver.onPacket(startPacket(60, metadata), 0, response);
    Packet cancel{};
    cancel.type = PacketType::Cancel;
    cancel.transfer_id = 60;
    cancel.error = ErrorCode::Cancelled;
    (void)receiver.onPacket(cancel, 1, response);
    TEST_ASSERT_TRUE(receiver.cleanupFailed());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ErrorCode::CleanupFailed),
                          static_cast<int>(receiver.error()));
    TEST_ASSERT_FALSE(receiver.reset());
    TEST_ASSERT_TRUE(receiver.cleanupFailed());
    TEST_ASSERT_TRUE(sink.partial_exists);
    sink.remove_ok = true;
    TEST_ASSERT_TRUE(receiver.reset());
    TEST_ASSERT_FALSE(sink.partial_exists);
}

void test_protocol_v2_sender_rejects_invalid_start_and_ready_rejection()
{
    FakeSource source;
    source.bytes = {1};
    const Metadata metadata = inspect(source);
    SenderSession sender(&source, sourceCallbacks());
    TEST_ASSERT_FALSE(sender.begin(0, metadata, 0));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ErrorCode::StateViolation),
                          static_cast<int>(sender.error()));
    TEST_ASSERT_TRUE(sender.begin(70, metadata, 0));
    Frame outbound{};
    TEST_ASSERT_TRUE(sender.outboundFrame(outbound));
    TEST_ASSERT_TRUE(sender.noteFrameSent(0));
    Packet ready{};
    ready.type = PacketType::Ready;
    ready.transfer_id = 70;
    ready.accepted = false;
    ready.error = ErrorCode::Busy;
    const Frame response = encoded(ready);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SenderEvent::Failed),
                          static_cast<int>(sender.onFrame(
                              response.data(), response.size(), 1)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ErrorCode::Busy),
                          static_cast<int>(sender.error()));
}

void test_protocol_v2_sender_ignores_wrong_transfer_and_requires_complete()
{
    FakeSource source;
    source.bytes = {1};
    const Metadata metadata = inspect(source);
    SenderSession sender(&source, sourceCallbacks());
    TEST_ASSERT_TRUE(sender.begin(80, metadata, 0));
    Frame frame{};
    (void)sender.outboundFrame(frame);
    (void)sender.noteFrameSent(0);
    Packet ready{};
    ready.type = PacketType::Ready;
    ready.transfer_id = 81;
    ready.accepted = true;
    ready.error = ErrorCode::None;
    Frame response = encoded(ready);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SenderEvent::Ignored),
                          static_cast<int>(sender.onFrame(
                              response.data(), response.size(), 1)));
    ready.transfer_id = 80;
    response = encoded(ready);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SenderEvent::OutboundReady),
                          static_cast<int>(sender.onFrame(
                              response.data(), response.size(), 2)));
    TEST_ASSERT_FALSE(sender.peerComplete());
    TEST_ASSERT_FALSE(sender.state() == SenderState::Completed);
}

void test_protocol_v2_sender_retry_timeout_and_exhaustion_are_bounded()
{
    FakeSource source;
    source.bytes = {1};
    const Metadata metadata = inspect(source);
    SenderSession sender(&source, sourceCallbacks(), 10, 5, 2, 1000);
    TEST_ASSERT_TRUE(sender.begin(90, metadata, 0));
    Frame frame{};
    TEST_ASSERT_TRUE(sender.outboundFrame(frame));
    TEST_ASSERT_TRUE(sender.noteFrameSent(0));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SenderEvent::NoChange),
                          static_cast<int>(sender.tick(9)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SenderEvent::OutboundReady),
                          static_cast<int>(sender.tick(10)));
    TEST_ASSERT_TRUE(sender.outboundFrame(frame));
    (void)sender.noteFrameSent(10);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SenderEvent::OutboundReady),
                          static_cast<int>(sender.tick(20)));
    (void)sender.outboundFrame(frame);
    (void)sender.noteFrameSent(20);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SenderEvent::Failed),
                          static_cast<int>(sender.tick(30)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ErrorCode::RetryExhausted),
                          static_cast<int>(sender.error()));
    TEST_ASSERT_EQUAL_UINT32(2, sender.totalRetries());
}

void test_protocol_v2_sender_overall_timeout_during_start()
{
    FakeSource source;
    source.bytes = {1};
    SenderSession sender(&source, sourceCallbacks(), 1000, 1000, 5, 50);
    TEST_ASSERT_TRUE(sender.begin(91, inspect(source), 0));
    Frame frame{};
    TEST_ASSERT_TRUE(sender.outboundFrame(frame));
    TEST_ASSERT_TRUE(sender.noteFrameSent(0));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SenderEvent::NoChange),
                          static_cast<int>(sender.tick(49)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SenderEvent::Failed),
                          static_cast<int>(sender.tick(50)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ErrorCode::Timeout),
                          static_cast<int>(sender.error()));
}

void test_protocol_v2_sender_overall_timeout_during_data()
{
    FakeSource source;
    source.bytes = {1};
    SenderSession sender(&source, sourceCallbacks(), 1000, 1000, 5, 50);
    TEST_ASSERT_TRUE(sender.begin(92, inspect(source), 0));
    Frame frame{};
    TEST_ASSERT_TRUE(sender.outboundFrame(frame));
    TEST_ASSERT_TRUE(sender.noteFrameSent(0));

    Packet ready{};
    ready.type = PacketType::Ready;
    ready.transfer_id = 92;
    ready.accepted = true;
    ready.error = ErrorCode::None;
    const Frame ready_frame = encoded(ready);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SenderEvent::OutboundReady),
                          static_cast<int>(sender.onFrame(
                              ready_frame.data(), ready_frame.size(), 1)));
    TEST_ASSERT_TRUE(sender.outboundFrame(frame));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(PacketType::Data),
                          static_cast<int>(decoded(frame).type));
    TEST_ASSERT_TRUE(sender.noteFrameSent(1));

    TEST_ASSERT_EQUAL_INT(static_cast<int>(SenderEvent::Ignored),
                          static_cast<int>(sender.onFrame(
                              ready_frame.data(), ready_frame.size(), 40)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SenderEvent::Failed),
                          static_cast<int>(sender.tick(51)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ErrorCode::Timeout),
                          static_cast<int>(sender.error()));
}

void test_protocol_v2_sender_overall_timeout_waiting_for_complete()
{
    FakeSource source;
    SenderSession sender(&source, sourceCallbacks(), 1000, 1000, 5, 50);
    TEST_ASSERT_TRUE(sender.begin(93, inspect(source), 0));
    Frame frame{};
    TEST_ASSERT_TRUE(sender.outboundFrame(frame));
    TEST_ASSERT_TRUE(sender.noteFrameSent(0));

    Packet ready{};
    ready.type = PacketType::Ready;
    ready.transfer_id = 93;
    ready.accepted = true;
    ready.error = ErrorCode::None;
    const Frame ready_frame = encoded(ready);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SenderEvent::OutboundReady),
                          static_cast<int>(sender.onFrame(
                              ready_frame.data(), ready_frame.size(), 1)));
    TEST_ASSERT_TRUE(sender.outboundFrame(frame));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(PacketType::End),
                          static_cast<int>(decoded(frame).type));
    TEST_ASSERT_TRUE(sender.noteFrameSent(1));

    Packet stale_ack{};
    stale_ack.type = PacketType::Ack;
    stale_ack.transfer_id = 93;
    stale_ack.sequence = 0;
    const Frame ack_frame = encoded(stale_ack);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SenderEvent::Ignored),
                          static_cast<int>(sender.onFrame(
                              ack_frame.data(), ack_frame.size(), 40)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SenderEvent::Failed),
                          static_cast<int>(sender.tick(51)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ErrorCode::Timeout),
                          static_cast<int>(sender.error()));
}

void test_protocol_v2_sender_cancellation_is_deterministic()
{
    FakeSource source;
    source.bytes = {1};
    SenderSession sender(&source, sourceCallbacks());
    TEST_ASSERT_TRUE(sender.begin(100, inspect(source), 0));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SenderEvent::OutboundReady),
                          static_cast<int>(sender.cancel(1)));
    Frame cancel{};
    TEST_ASSERT_TRUE(sender.outboundFrame(cancel));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(PacketType::Cancel),
                          static_cast<int>(decoded(cancel).type));
    (void)sender.noteFrameSent(1);
    Packet error{};
    error.type = PacketType::Error;
    error.transfer_id = 100;
    error.error = ErrorCode::Cancelled;
    const Frame response = encoded(error);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SenderEvent::Cancelled),
                          static_cast<int>(sender.onFrame(
                              response.data(), response.size(), 2)));
}

void test_protocol_v2_end_to_end_zero_one_exact_full_and_binary_files()
{
    const std::vector<std::vector<uint8_t>> fixtures = {
        {}, {0x00}, std::vector<uint8_t>(20, 0xFF),
        std::vector<uint8_t>(40, 0x00),
        {0x00, 0xFF, 0x01, 0x80, 0x7F, 0x55, 0xAA}};
    for (const auto& fixture : fixtures) {
        FakeSource source;
        source.bytes = fixture;
        FakeSink sink;
        Harness harness(source, sink);
        TEST_ASSERT_TRUE(harness.begin());
        assertSuccessfulTransfer(harness);
        if (!fixture.empty()) {
            TEST_ASSERT_EQUAL_UINT8_ARRAY(
                fixture.data(), sink.bytes.data(), fixture.size());
        }
    }
}

void test_protocol_v2_end_to_end_all_byte_values_and_seeded_random_data()
{
    FakeSource source;
    for (uint32_t value = 0; value < 256; ++value) {
        source.bytes.push_back(static_cast<uint8_t>(value));
    }
    uint32_t random = 0x12345678u;
    for (size_t index = 0; index < 513; ++index) {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        source.bytes.push_back(static_cast<uint8_t>(random));
    }
    FakeSink sink;
    Harness harness(source, sink, 0xCAFEBABEu);
    TEST_ASSERT_TRUE(harness.begin());
    assertSuccessfulTransfer(harness);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(source.bytes.data(), sink.bytes.data(), source.bytes.size());
}

FaultRule dropRule(Destination destination,
                   PacketType type,
                   uint32_t sequence = 0,
                   bool any_sequence = true,
                   uint32_t repeat_count = 1)
{
    FaultRule rule{};
    rule.destination = destination;
    rule.kind = FaultKind::Drop;
    rule.type = type;
    rule.sequence = sequence;
    rule.any_sequence = any_sequence;
    rule.repeat_count = repeat_count;
    return rule;
}

void test_protocol_v2_lost_start_ready_data_ack_end_and_complete_recover()
{
    struct Scenario {
        Destination destination;
        PacketType type;
        uint32_t sequence;
        bool any_sequence;
    };
    const Scenario scenarios[] = {
        {Destination::Receiver, PacketType::Start, 0, true},
        {Destination::Sender, PacketType::Ready, 0, true},
        {Destination::Receiver, PacketType::Data, 0, false},
        {Destination::Sender, PacketType::Ack, 0, false},
        {Destination::Receiver, PacketType::End, 0, true},
        {Destination::Sender, PacketType::Complete, 0, true},
    };
    for (const Scenario& scenario : scenarios) {
        FakeSource source;
        source.generated = true;
        source.generated_size = 45;
        FakeSink sink;
        Harness harness(source, sink);
        TEST_ASSERT_TRUE(harness.transport.addRule(dropRule(
            scenario.destination, scenario.type,
            scenario.sequence, scenario.any_sequence)));
        TEST_ASSERT_TRUE(harness.begin());
        assertSuccessfulTransfer(harness);
        TEST_ASSERT_TRUE(harness.sender.totalRetries() >= 1);
        TEST_ASSERT_EQUAL_UINT32(45, sink.received_size);
    }
}

void test_protocol_v2_duplicate_start_data_ack_and_end_are_idempotent()
{
    struct Scenario { Destination destination; PacketType type; uint32_t sequence; bool any; };
    const Scenario scenarios[] = {
        {Destination::Receiver, PacketType::Start, 0, true},
        {Destination::Receiver, PacketType::Data, 0, false},
        {Destination::Sender, PacketType::Ack, 0, false},
        {Destination::Receiver, PacketType::End, 0, true},
    };
    for (const Scenario& scenario : scenarios) {
        FakeSource source;
        source.generated = true;
        source.generated_size = 25;
        FakeSink sink;
        Harness harness(source, sink);
        FaultRule rule{};
        rule.destination = scenario.destination;
        rule.kind = FaultKind::Duplicate;
        rule.type = scenario.type;
        rule.sequence = scenario.sequence;
        rule.any_sequence = scenario.any;
        TEST_ASSERT_TRUE(harness.transport.addRule(rule));
        TEST_ASSERT_TRUE(harness.begin());
        assertSuccessfulTransfer(harness);
        TEST_ASSERT_EQUAL_UINT32(25, sink.received_size);
        TEST_ASSERT_EQUAL_UINT32(2, sink.write_count);
        TEST_ASSERT_EQUAL_UINT32(1, sink.publish_count);
    }
}

void test_protocol_v2_corrupted_data_is_detected_by_end_to_end_crc()
{
    FakeSource source;
    source.generated = true;
    source.generated_size = 30;
    FakeSink sink;
    Harness harness(source, sink);
    FaultRule rule{};
    rule.destination = Destination::Receiver;
    rule.kind = FaultKind::Corrupt;
    rule.type = PacketType::Data;
    rule.any_sequence = false;
    rule.sequence = 0;
    rule.corrupt_offset = ProtocolV2::kDataHeaderBytes;
    rule.corrupt_mask = 0x80;
    TEST_ASSERT_TRUE(harness.transport.addRule(rule));
    TEST_ASSERT_TRUE(harness.begin());
    TEST_ASSERT_FALSE(harness.run());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SenderState::Failed),
                          static_cast<int>(harness.sender.state()));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ErrorCode::CrcMismatch),
                          static_cast<int>(harness.sender.error()));
    TEST_ASSERT_FALSE(sink.published);
    TEST_ASSERT_FALSE(sink.partial_exists);
}

void test_protocol_v2_retry_exhaustion_fails_without_publication()
{
    FakeSource source;
    source.generated = true;
    source.generated_size = 25;
    FakeSink sink;
    Harness harness(source, sink);
    TEST_ASSERT_TRUE(harness.transport.addRule(dropRule(
        Destination::Sender, PacketType::Ack, 0, false,
        static_cast<uint32_t>(ProtocolV2::kMaximumRetries) + 1u)));
    TEST_ASSERT_TRUE(harness.begin());
    TEST_ASSERT_FALSE(harness.run());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ErrorCode::RetryExhausted),
                          static_cast<int>(harness.sender.error()));
    TEST_ASSERT_FALSE(sink.published);
    TEST_ASSERT_TRUE(sink.partial_exists);
    Frame response{};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ReceiverEvent::TimedOut),
                          static_cast<int>(harness.receiver.tick(
                              harness.now_ms + ProtocolV2::kReceiverInactivityTimeoutMs,
                              response)));
    TEST_ASSERT_FALSE(sink.partial_exists);
}

void test_protocol_v2_delay_and_reordering_support_is_deterministic()
{
    FakeSource source;
    source.generated = true;
    source.generated_size = 22;
    FakeSink sink;
    Harness harness(source, sink, 0x11111111u);
    FaultRule delay{};
    delay.destination = Destination::Sender;
    delay.kind = FaultKind::Delay;
    delay.type = PacketType::Ack;
    delay.any_sequence = false;
    delay.sequence = 0;
    delay.delay_ms = ProtocolV2::kDataAckTimeoutMs + 20;
    TEST_ASSERT_TRUE(harness.transport.addRule(delay));
    TEST_ASSERT_TRUE(harness.begin());
    assertSuccessfulTransfer(harness);
    TEST_ASSERT_EQUAL_UINT32(22, sink.received_size);
    TEST_ASSERT_TRUE(harness.sender.totalRetries() >= 1);
}

void test_protocol_v2_fake_transport_supports_selective_rules_and_endpoint_resets()
{
    FakeDuplexTransport transport(7);
    Packet packet{};
    packet.type = PacketType::Ack;
    packet.transfer_id = 1;
    packet.sequence = 9;
    const Frame frame = encoded(packet);
    TEST_ASSERT_TRUE(transport.send(Destination::Sender, frame, 0));
    TEST_ASSERT_EQUAL_UINT32(1, transport.queuedFrames());
    transport.resetSender();
    TEST_ASSERT_EQUAL_UINT32(0, transport.queuedFrames());
    TEST_ASSERT_EQUAL_UINT32(1, transport.senderResetCount());
    TEST_ASSERT_TRUE(transport.send(Destination::Sender, frame, 0));
    transport.resetReceiver();
    TEST_ASSERT_EQUAL_UINT32(0, transport.queuedFrames());
    TEST_ASSERT_EQUAL_UINT32(1, transport.receiverResetCount());
}

void test_protocol_v2_maximum_transfer_streams_without_full_allocation()
{
    FakeSource source;
    source.generated = true;
    source.generated_size = ProtocolV2::kMaxFileSize;
    FakeSink sink;
    sink.collect = false;
    sink.validate_pattern = true;
    Harness harness(source, sink);
    TEST_ASSERT_TRUE(harness.begin());
    assertSuccessfulTransfer(harness);
    TEST_ASSERT_TRUE(source.bytes.empty());
    TEST_ASSERT_TRUE(sink.bytes.empty());
    TEST_ASSERT_TRUE(sink.pattern_valid);
    TEST_ASSERT_EQUAL_UINT32(ProtocolV2::kMaxFileSize, sink.received_size);
    TEST_ASSERT_EQUAL_UINT32(ProtocolV2::kMaxPacketCount,
                             harness.receiver.acceptedPackets());
    TEST_ASSERT_EQUAL_UINT32(65536, harness.sender.currentSequence());
}

void test_protocol_v2_back_to_back_transfers_and_collision_safe_adapter_contract()
{
    FakeSink sink;
    FakeSource first_source;
    first_source.bytes = {1, 2, 3};
    Harness first(first_source, sink);
    first.transfer_id = 200;
    TEST_ASSERT_TRUE(first.begin());
    assertSuccessfulTransfer(first);
    const uint32_t first_publish_count = sink.publish_count;
    TEST_ASSERT_TRUE(sink.existing_completed_intact);

    FakeSource second_source;
    second_source.bytes = {4, 5, 6, 7};
    Harness second(second_source, sink);
    second.transfer_id = 201;
    TEST_ASSERT_TRUE(second.begin());
    assertSuccessfulTransfer(second);
    TEST_ASSERT_EQUAL_UINT32(first_publish_count + 1u, sink.publish_count);
    TEST_ASSERT_TRUE(sink.existing_completed_intact);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(second_source.bytes.data(),
                                  sink.bytes.data(), second_source.bytes.size());
}

}  // namespace

void runProtocolV2Tests()
{
    RUN_TEST(test_protocol_v2_constants_and_size_limits);
    RUN_TEST(test_protocol_v2_start_round_trip_and_little_endian_layout);
    RUN_TEST(test_protocol_v2_every_control_packet_round_trips);
    RUN_TEST(test_protocol_v2_data_round_trip_preserves_binary_and_zero_padding);
    RUN_TEST(test_protocol_v2_decode_rejects_magic_version_type_and_length_errors);
    RUN_TEST(test_protocol_v2_decode_rejects_nonzero_reserved_and_padding_bytes);
    RUN_TEST(test_protocol_v2_rejects_unknown_error_codes);
    RUN_TEST(test_protocol_v2_encode_rejects_zero_id_invalid_metadata_and_zero_data);
    RUN_TEST(test_protocol_v2_crc32_known_vectors);
    RUN_TEST(test_protocol_v2_crc32_incremental_matches_one_pass);
    RUN_TEST(test_protocol_v2_source_inspection_covers_zero_boundaries_and_partial_reads);
    RUN_TEST(test_protocol_v2_source_inspection_maximum_is_incremental);
    RUN_TEST(test_protocol_v2_source_inspection_rejects_over_limit_without_full_buffer);
    RUN_TEST(test_protocol_v2_source_inspection_propagates_read_and_reset_errors);
    RUN_TEST(test_protocol_v2_receiver_start_ready_duplicate_and_busy_behavior);
    RUN_TEST(test_protocol_v2_receiver_data_gap_duplicate_and_wrong_id_are_safe);
    RUN_TEST(test_protocol_v2_receiver_valid_end_publishes_and_duplicate_end_replies_complete);
    RUN_TEST(test_protocol_v2_receiver_crc_mismatch_and_extra_data_never_publish);
    RUN_TEST(test_protocol_v2_receiver_cancel_timeout_and_new_transfer_cleanup);
    RUN_TEST(test_protocol_v2_receiver_zero_byte_file_is_verified_and_published);
    RUN_TEST(test_protocol_v2_receiver_storage_failures_preserve_existing_completed_file);
    RUN_TEST(test_protocol_v2_receiver_cleanup_failure_is_reported_and_retryable);
    RUN_TEST(test_protocol_v2_sender_rejects_invalid_start_and_ready_rejection);
    RUN_TEST(test_protocol_v2_sender_ignores_wrong_transfer_and_requires_complete);
    RUN_TEST(test_protocol_v2_sender_retry_timeout_and_exhaustion_are_bounded);
    RUN_TEST(test_protocol_v2_sender_overall_timeout_during_start);
    RUN_TEST(test_protocol_v2_sender_overall_timeout_during_data);
    RUN_TEST(test_protocol_v2_sender_overall_timeout_waiting_for_complete);
    RUN_TEST(test_protocol_v2_sender_cancellation_is_deterministic);
    RUN_TEST(test_protocol_v2_end_to_end_zero_one_exact_full_and_binary_files);
    RUN_TEST(test_protocol_v2_end_to_end_all_byte_values_and_seeded_random_data);
    RUN_TEST(test_protocol_v2_lost_start_ready_data_ack_end_and_complete_recover);
    RUN_TEST(test_protocol_v2_duplicate_start_data_ack_and_end_are_idempotent);
    RUN_TEST(test_protocol_v2_corrupted_data_is_detected_by_end_to_end_crc);
    RUN_TEST(test_protocol_v2_retry_exhaustion_fails_without_publication);
    RUN_TEST(test_protocol_v2_delay_and_reordering_support_is_deterministic);
    RUN_TEST(test_protocol_v2_fake_transport_supports_selective_rules_and_endpoint_resets);
    RUN_TEST(test_protocol_v2_maximum_transfer_streams_without_full_allocation);
    RUN_TEST(test_protocol_v2_back_to_back_transfers_and_collision_safe_adapter_contract);
}
