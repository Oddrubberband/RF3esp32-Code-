#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <vector>

#include <unity.h>

#include "../include/fake_hal.hpp"
#include "audio_packet.hpp"
#include "audio_reassembler.hpp"
#include "file_segmenter.hpp"
#include "frame_io.hpp"
#include "morse.hpp"
#include "nrf24.hpp"
#include "radio_manager.hpp"
#include "rx_drain.hpp"
#include "tx_helpers.hpp"
#include "validation.hpp"
#include "stream_sync.hpp"

void runReceiverSafetyTests();
void runProtocolV2Tests();
void runIntegrationReadinessTests();

void setUp(void)
{
}

void tearDown(void)
{
}

class TimeoutHal : public FakeHal {
public:
    void ce(bool level) override
    {
        const bool rising = !ce_level && level;
        ce_level = level;

        const bool prim_rx = (regs[0x00] & (1 << 0)) != 0;
        const bool power_up = (regs[0x00] & (1 << 1)) != 0;

        // Count attempted TX launches, but never raise TX_DS or MAX_RT.
        // This forces transmitOnce() down its timeout path.
        if (rising && power_up && !prim_rx && !tx_fifo.empty()) {
            ++tx_trigger_count;
        }
    }
};

class ProbeFailHal : public FakeHal {
public:
    void spiTxRx(const uint8_t* tx, uint8_t* rx, size_t n) override
    {
        const uint8_t old_rf_ch = regs[0x05];
        FakeHal::spiTxRx(tx, rx, n);

        // Make RF_CH writes appear to fail so probe() cannot read back the test value.
        if (n > 0 && (tx[0] & 0xE0) == 0x20 && (tx[0] & 0x1F) == 0x05) {
            regs[0x05] = old_rf_ch;
        }
    }
};
// Small helper so the individual tests stay focused on behavior instead of loop boilerplate.
static void assertBytes(const std::vector<uint8_t>& actual, std::initializer_list<uint8_t> expected)
{
    TEST_ASSERT_EQUAL(static_cast<int>(expected.size()), static_cast<int>(actual.size()));

    size_t index = 0;
    for (uint8_t value : expected) {
        TEST_ASSERT_EQUAL_UINT8(value, actual[index]);
        ++index;
    }
}

namespace {

using FileSegmentation::NextResult;
using FileSegmentation::ReadResult;
using FileSegmentation::ReadState;
using FileSegmentation::Segment;
using FileSegmentation::Segmenter;

struct BufferReadContext {
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t offset = 0;

    BufferReadContext(const uint8_t* source_data = nullptr,
                      size_t source_size = 0,
                      size_t source_offset = 0)
        : data(source_data), size(source_size), offset(source_offset)
    {
    }
};

ReadResult readBuffer(void* context, uint8_t* out, size_t capacity)
{
    BufferReadContext* source = static_cast<BufferReadContext*>(context);
    if (!source || !out || capacity == 0) {
        return {0, ReadState::Error};
    }
    if (source->offset == source->size) {
        return {0, ReadState::EndOfFile};
    }

    const size_t remaining = source->size - source->offset;
    const size_t count = std::min(capacity, remaining);
    std::copy(source->data + source->offset,
              source->data + source->offset + count,
              out);
    source->offset += count;

    // Match fread(): EOF is known on this read only when the read is short.
    const ReadState state = count < capacity ? ReadState::EndOfFile
                                             : ReadState::MoreDataMayFollow;
    return {count, state};
}

ReadResult readError(void*, uint8_t*, size_t)
{
    return {0, ReadState::Error};
}

uint8_t generatedByte(uint64_t offset)
{
    return static_cast<uint8_t>((offset * 131u + 17u) & 0xFFu);
}

struct GeneratedReadContext {
    uint64_t size = 0;
    uint64_t offset = 0;

    GeneratedReadContext(uint64_t source_size, uint64_t source_offset)
        : size(source_size), offset(source_offset)
    {
    }
};

ReadResult readGenerated(void* context, uint8_t* out, size_t capacity)
{
    GeneratedReadContext* source = static_cast<GeneratedReadContext*>(context);
    if (!source || !out || capacity == 0) {
        return {0, ReadState::Error};
    }
    if (source->offset == source->size) {
        return {0, ReadState::EndOfFile};
    }

    const uint64_t remaining = source->size - source->offset;
    const size_t count = static_cast<size_t>(
        std::min<uint64_t>(remaining, static_cast<uint64_t>(capacity)));
    for (size_t index = 0; index < count; ++index) {
        out[index] = generatedByte(source->offset + index);
    }
    source->offset += count;

    const ReadState state = count < capacity ? ReadState::EndOfFile
                                             : ReadState::MoreDataMayFollow;
    return {count, state};
}

std::vector<uint8_t> incrementingFixture(size_t size)
{
    std::vector<uint8_t> data(size);
    for (size_t index = 0; index < data.size(); ++index) {
        data[index] = static_cast<uint8_t>(index & 0xFFu);
    }
    return data;
}

void assertSegmentedFixture(const std::vector<uint8_t>& source)
{
    TEST_ASSERT_FALSE(source.empty());

    BufferReadContext read_context{source.data(), source.size(), 0};
    Segmenter segmenter(&read_context, &readBuffer);
    const size_t expected_count =
        (source.size() + AudioPacket::kAudioBytesPerPacket - 1u) /
        AudioPacket::kAudioBytesPerPacket;
    std::vector<uint8_t> reconstructed;
    reconstructed.reserve(source.size());

    for (size_t packet_index = 0; packet_index < expected_count; ++packet_index) {
        Segment segment{};
        TEST_ASSERT_EQUAL_INT(static_cast<int>(NextResult::SegmentReady),
                              static_cast<int>(segmenter.next(segment)));
        TEST_ASSERT_EQUAL_UINT16(static_cast<uint16_t>(packet_index), segment.sequence);
        TEST_ASSERT_EQUAL(packet_index == 0, segment.first);

        const size_t source_offset = packet_index * AudioPacket::kAudioBytesPerPacket;
        const size_t expected_length = std::min<size_t>(
            AudioPacket::kAudioBytesPerPacket, source.size() - source_offset);
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(expected_length),
                                segment.payload_length);

        for (size_t index = 0; index < expected_length; ++index) {
            TEST_ASSERT_EQUAL_UINT8(source[source_offset + index], segment.payload[index]);
            reconstructed.push_back(segment.payload[index]);
        }
        TEST_ASSERT_EQUAL(packet_index + 1u == expected_count, segment.last);
    }

    Segment unused{};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(NextResult::EndOfFile),
                          static_cast<int>(segmenter.next(unused)));
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(source.size()),
                             static_cast<uint32_t>(reconstructed.size()));
    for (size_t index = 0; index < source.size(); ++index) {
        TEST_ASSERT_EQUAL_UINT8(source[index], reconstructed[index]);
    }
}

}  // namespace

void test_fileSegmenter_empty_input_is_explicitly_rejected(void)
{
    BufferReadContext read_context{};
    Segmenter segmenter(&read_context, &readBuffer);
    Segment segment{};

    TEST_ASSERT_EQUAL_INT(static_cast<int>(NextResult::EmptyInput),
                          static_cast<int>(segmenter.next(segment)));
}

void test_fileSegmenter_read_error_is_distinct_from_clean_eof(void)
{
    Segmenter segmenter(nullptr, &readError);
    Segment segment{};

    TEST_ASSERT_EQUAL_INT(static_cast<int>(NextResult::ReadError),
                          static_cast<int>(segmenter.next(segment)));
}

void test_fileSegmenter_one_byte_round_trips(void)
{
    assertSegmentedFixture(incrementingFixture(1));
}

void test_fileSegmenter_27_bytes_round_trip(void)
{
    assertSegmentedFixture(incrementingFixture(27));
}

void test_fileSegmenter_exact_28_bytes_marks_final_segment_last(void)
{
    assertSegmentedFixture(incrementingFixture(28));
}

void test_fileSegmenter_29_bytes_round_trip(void)
{
    assertSegmentedFixture(incrementingFixture(29));
}

void test_fileSegmenter_55_bytes_round_trip(void)
{
    assertSegmentedFixture(incrementingFixture(55));
}

void test_fileSegmenter_exact_56_bytes_marks_final_segment_last(void)
{
    assertSegmentedFixture(incrementingFixture(56));
}

void test_fileSegmenter_57_bytes_round_trip(void)
{
    assertSegmentedFixture(incrementingFixture(57));
}

void test_fileSegmenter_binary_fixtures_round_trip(void)
{
    assertSegmentedFixture(std::vector<uint8_t>(57, 0x00));
    assertSegmentedFixture(std::vector<uint8_t>(57, 0xFF));
    assertSegmentedFixture(incrementingFixture(256));

    std::vector<uint8_t> seeded_random(1025);
    uint32_t state = 0x6D2B79F5u;
    for (uint8_t& value : seeded_random) {
        state = state * 1664525u + 1013904223u;
        value = static_cast<uint8_t>(state >> 24u);
    }
    assertSegmentedFixture(seeded_random);
}

void test_fileSegmenter_maximum_size_completes_with_final_last(void)
{
    constexpr uint64_t kPacketCount = static_cast<uint64_t>(UINT16_MAX) + 1u;
    constexpr uint64_t kMaximumBytes =
        kPacketCount * AudioPacket::kAudioBytesPerPacket;
    GeneratedReadContext read_context{kMaximumBytes, 0};
    Segmenter segmenter(&read_context, &readGenerated);

    for (uint32_t packet_index = 0; packet_index < kPacketCount; ++packet_index) {
        Segment segment{};
        TEST_ASSERT_EQUAL_INT(static_cast<int>(NextResult::SegmentReady),
                              static_cast<int>(segmenter.next(segment)));
        TEST_ASSERT_EQUAL_UINT16(static_cast<uint16_t>(packet_index), segment.sequence);
        TEST_ASSERT_EQUAL_UINT8(AudioPacket::kAudioBytesPerPacket,
                                segment.payload_length);
        TEST_ASSERT_EQUAL(packet_index == 0, segment.first);

        const uint64_t source_offset =
            static_cast<uint64_t>(packet_index) * AudioPacket::kAudioBytesPerPacket;
        for (size_t index = 0; index < segment.payload_length; ++index) {
            TEST_ASSERT_EQUAL_UINT8(generatedByte(source_offset + index),
                                    segment.payload[index]);
        }
        TEST_ASSERT_EQUAL_MESSAGE(packet_index + 1u == kPacketCount,
                                  segment.last,
                                  "maximum-size final segment must be LAST");
    }

    Segment unused{};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(NextResult::EndOfFile),
                          static_cast<int>(segmenter.next(unused)));
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(kMaximumBytes),
                             static_cast<uint32_t>(read_context.offset));
}

void test_fileSegmenter_over_maximum_is_rejected_before_sequence_wrap(void)
{
    constexpr uint64_t kPacketCount = static_cast<uint64_t>(UINT16_MAX) + 1u;
    constexpr uint64_t kMaximumBytes =
        kPacketCount * AudioPacket::kAudioBytesPerPacket;
    GeneratedReadContext read_context{kMaximumBytes + 1u, 0};
    Segmenter segmenter(&read_context, &readGenerated);

    for (uint32_t packet_index = 0; packet_index < kPacketCount; ++packet_index) {
        Segment segment{};
        TEST_ASSERT_EQUAL_INT(static_cast<int>(NextResult::SegmentReady),
                              static_cast<int>(segmenter.next(segment)));
        TEST_ASSERT_EQUAL_UINT16(static_cast<uint16_t>(packet_index), segment.sequence);
        TEST_ASSERT_EQUAL_UINT8(AudioPacket::kAudioBytesPerPacket,
                                segment.payload_length);
        TEST_ASSERT_EQUAL(packet_index == 0, segment.first);
        TEST_ASSERT_FALSE(segment.last);

        const uint64_t source_offset =
            static_cast<uint64_t>(packet_index) * AudioPacket::kAudioBytesPerPacket;
        for (size_t index = 0; index < segment.payload_length; ++index) {
            TEST_ASSERT_EQUAL_UINT8(generatedByte(source_offset + index),
                                    segment.payload[index]);
        }
    }

    Segment overflow{};
    TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(NextResult::UnsupportedSize),
                                  static_cast<int>(segmenter.next(overflow)),
                                  "input beyond 65,536 packets must be rejected");
}

void test_readReg_reads_value_and_formats_spi_command(void)
{
    FakeHal hal;
    hal.regs[0x07] = 0xAB;

    Nrf24 radio(hal);
    const uint8_t value = radio.readReg(0x07);

    TEST_ASSERT_EQUAL_UINT8(0xAB, value);
    TEST_ASSERT_EQUAL_UINT8(0x07, hal.last_tx[0]);
    TEST_ASSERT_EQUAL_UINT8(0xFF, hal.last_tx[1]);
}

void test_readRfPowerLevel_decodes_rf_setup_bits(void)
{
    FakeHal hal;
    hal.regs[0x06] = 0x94;

    Nrf24 radio(hal);

    TEST_ASSERT_EQUAL_UINT8(2, radio.readRfPowerLevel());
}

void test_setRfPowerLevel_updates_packet_setup_and_register(void)
{
    FakeHal hal;
    Nrf24 radio(hal);

    TEST_ASSERT_TRUE(radio.setRfPowerLevel(1));
    TEST_ASSERT_EQUAL_UINT8(0x02, static_cast<uint8_t>(hal.regs[0x06] & 0x06));
    TEST_ASSERT_EQUAL_UINT8(1, radio.readRfPowerLevel());
}

// Audio packet / reassembly tests
void test_audioPacket_encode_decode_round_trip(void)
{
    const uint8_t audio[] = {0x12, 0x34, 0x56};
    uint8_t packet[AudioPacket::kPacketBytes] = {};
    size_t packet_len = 0;

    const bool encoded = AudioPacket::encode(7, audio, sizeof(audio), true, false, packet, packet_len);
    TEST_ASSERT_TRUE(encoded);
    TEST_ASSERT_EQUAL_UINT32(AudioPacket::kPacketBytes, static_cast<uint32_t>(packet_len));
    for (size_t i = AudioPacket::kHeaderBytes + sizeof(audio); i < AudioPacket::kPacketBytes; ++i) {
        TEST_ASSERT_EQUAL_UINT8(0, packet[i]);
    }

    AudioPacket::Header header;
    const uint8_t* decoded_audio = nullptr;
    const bool decoded = AudioPacket::decode(packet, packet_len, header, decoded_audio);

    TEST_ASSERT_TRUE(decoded);
    TEST_ASSERT_EQUAL_UINT16(7, header.sequence);
    TEST_ASSERT_EQUAL_UINT8(sizeof(audio), header.audio_len);
    TEST_ASSERT_EQUAL_UINT8(AudioPacket::kFirst, header.flags);
    assertBytes(std::vector<uint8_t>(decoded_audio, decoded_audio + header.audio_len), {0x12, 0x34, 0x56});
}

void test_audioPacket_rejects_oversized_audio(void)
{
    uint8_t audio[AudioPacket::kAudioBytesPerPacket + 1] = {};
    uint8_t packet[AudioPacket::kPacketBytes] = {};
    size_t packet_len = 0;

    const bool ok = AudioPacket::encode(0, audio, sizeof(audio), true, false, packet, packet_len);

    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(packet_len));
}

void test_audioReassembler_reassembles_packets_in_order(void)
{
    uint8_t packet0[AudioPacket::kPacketBytes] = {};
    uint8_t packet1[AudioPacket::kPacketBytes] = {};
    uint8_t packet2[AudioPacket::kPacketBytes] = {};
    size_t len0 = 0;
    size_t len1 = 0;
    size_t len2 = 0;

    TEST_ASSERT_TRUE(AudioPacket::encode(0, reinterpret_cast<const uint8_t*>("abc"), 3, true, false, packet0, len0));
    TEST_ASSERT_TRUE(AudioPacket::encode(1, reinterpret_cast<const uint8_t*>("def"), 3, false, false, packet1, len1));
    TEST_ASSERT_TRUE(AudioPacket::encode(2, reinterpret_cast<const uint8_t*>("gh"), 2, false, true, packet2, len2));

    AudioReassembler reassembler;
    TEST_ASSERT_TRUE(reassembler.acceptPacket(packet0, len0));
    TEST_ASSERT_TRUE(reassembler.acceptPacket(packet1, len1));
    TEST_ASSERT_TRUE(reassembler.acceptPacket(packet2, len2));

    TEST_ASSERT_TRUE(reassembler.started());
    TEST_ASSERT_TRUE(reassembler.complete());
    TEST_ASSERT_EQUAL_UINT16(3, reassembler.nextSequence());
    assertBytes(reassembler.audio(), {'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h'});
}

void test_audioReassembler_accepts_small_forward_gap_and_records_missing(void)
{
    uint8_t packet0[AudioPacket::kPacketBytes] = {};
    uint8_t packet2[AudioPacket::kPacketBytes] = {};
    size_t len0 = 0;
    size_t len2 = 0;

    TEST_ASSERT_TRUE(AudioPacket::encode(0, reinterpret_cast<const uint8_t*>("abc"), 3, true, false, packet0, len0));
    TEST_ASSERT_TRUE(AudioPacket::encode(2, reinterpret_cast<const uint8_t*>("zz"), 2, false, true, packet2, len2));

    AudioReassembler reassembler;
    TEST_ASSERT_TRUE(reassembler.acceptPacket(packet0, len0));
    TEST_ASSERT_TRUE(reassembler.acceptPacket(packet2, len2));
    TEST_ASSERT_TRUE(reassembler.complete());
    TEST_ASSERT_EQUAL_UINT16(3, reassembler.nextSequence());
    TEST_ASSERT_EQUAL_UINT32(1, reassembler.missingPackets());
    assertBytes(reassembler.audio(), {'a', 'b', 'c', 'z', 'z'});
}

void test_audioReassembler_rejects_large_forward_gap_and_resets(void)
{
    uint8_t packet0[AudioPacket::kPacketBytes] = {};
    uint8_t packet10[AudioPacket::kPacketBytes] = {};
    size_t len0 = 0;
    size_t len10 = 0;

    TEST_ASSERT_TRUE(AudioPacket::encode(0, reinterpret_cast<const uint8_t*>("abc"), 3, true, false, packet0, len0));
    TEST_ASSERT_TRUE(AudioPacket::encode(10, reinterpret_cast<const uint8_t*>("zz"), 2, false, true, packet10, len10));

    AudioReassembler reassembler;
    TEST_ASSERT_TRUE(reassembler.acceptPacket(packet0, len0));
    TEST_ASSERT_FALSE(reassembler.acceptPacket(packet10, len10));
    TEST_ASSERT_EQUAL(static_cast<int>(AudioReassemblyError::SequenceGapTooLarge),
                      static_cast<int>(reassembler.lastError()));
    TEST_ASSERT_FALSE(reassembler.started());
    TEST_ASSERT_EQUAL_UINT16(0, reassembler.nextSequence());
    TEST_ASSERT_EQUAL_UINT32(0, reassembler.missingPackets());
}

void test_audioReassembler_ignores_duplicate_packet(void)
{
    uint8_t packet0[AudioPacket::kPacketBytes] = {};
    size_t len0 = 0;

    TEST_ASSERT_TRUE(AudioPacket::encode(0, reinterpret_cast<const uint8_t*>("abc"), 3, true, false, packet0, len0));

    AudioReassembler reassembler;
    TEST_ASSERT_TRUE(reassembler.acceptPacket(packet0, len0));
    TEST_ASSERT_FALSE(reassembler.acceptPacket(packet0, len0));
    TEST_ASSERT_EQUAL(static_cast<int>(AudioReassemblyError::DuplicateOrOld),
                      static_cast<int>(reassembler.lastError()));
    TEST_ASSERT_EQUAL_UINT16(1, reassembler.nextSequence());
    TEST_ASSERT_EQUAL_UINT32(0, reassembler.missingPackets());
    assertBytes(reassembler.audio(), {'a', 'b', 'c'});
}

// Driver and manager tests
void test_writeReg_writes_register_and_formats_spi_command(void)
{
    FakeHal hal;
    Nrf24 radio(hal);

    radio.writeReg(0x05, 76);

    TEST_ASSERT_EQUAL_UINT8(76, hal.regs[0x05]);
    TEST_ASSERT_EQUAL_UINT8(0x25, hal.last_tx[0]);
    TEST_ASSERT_EQUAL_UINT8(76, hal.last_tx[1]);
}

void test_powerUp_sets_power_bit_and_waits_for_startup(void)
{
    FakeHal hal;
    hal.regs[0x00] = 0x00;

    Nrf24 radio(hal);
    const bool ok = radio.powerUp();

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_TRUE((hal.regs[0x00] & (1 << 1)) != 0);
    TEST_ASSERT_EQUAL_UINT32(1500, static_cast<uint32_t>(hal.time_us));
}

void test_probe_restores_original_channel_after_check(void)
{
    FakeHal hal;
    hal.regs[0x05] = 76;

    Nrf24 radio(hal);
    const bool ok = radio.probe();

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_UINT8(76, hal.regs[0x05]);
}

void test_initDefaults_programs_expected_registers(void)
{
    FakeHal hal;
    Nrf24 radio(hal);

    const bool ok = radio.initDefaults(40);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_UINT8(0x0E, hal.regs[0x00]);
    TEST_ASSERT_EQUAL_UINT8(0x00, hal.regs[0x04]);
    TEST_ASSERT_EQUAL_UINT8(0x01, hal.regs[0x02]);
    TEST_ASSERT_EQUAL_UINT8(40, hal.regs[0x05]);
    TEST_ASSERT_EQUAL_UINT8(0x26, hal.regs[0x06]);
    TEST_ASSERT_EQUAL_UINT8(32, hal.regs[0x11]);
    TEST_ASSERT_FALSE(hal.ce_level);
}

void test_startRx_sets_rx_mode_and_raises_ce(void)
{
    FakeHal hal;
    Nrf24 radio(hal);

    const bool ok = radio.startRx();

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_TRUE((hal.regs[0x00] & (1 << 1)) != 0);
    TEST_ASSERT_TRUE((hal.regs[0x00] & (1 << 0)) != 0);
    TEST_ASSERT_TRUE(hal.ce_level);
}

void test_transmitOnce_success_writes_payload_and_reports_success(void)
{
    FakeHal hal;
    hal.next_tx_success = true;

    Nrf24 radio(hal);
    const uint8_t payload[] = {0x11, 0x22, 0x33};

    const bool ok = radio.transmitOnce(payload, sizeof(payload), 1000);

    TEST_ASSERT_TRUE(ok);
    assertBytes(hal.last_payload_write, {0x11, 0x22, 0x33});
    TEST_ASSERT_EQUAL(1, hal.tx_trigger_count);
    TEST_ASSERT_TRUE(hal.tx_fifo.empty());
    TEST_ASSERT_TRUE(radio.lastTxSawIrq());
    TEST_ASSERT_EQUAL_UINT8(0, static_cast<uint8_t>(hal.regs[0x07] & (1 << 5)));
}

void test_transmitOnce_failure_clears_fifo_and_returns_false(void)
{
    FakeHal hal;
    hal.next_tx_success = false;

    Nrf24 radio(hal);
    const uint8_t payload[] = {0xAA, 0xBB};

    const bool ok = radio.transmitOnce(payload, sizeof(payload), 1000);

    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_TRUE(hal.tx_fifo.empty());
    TEST_ASSERT_EQUAL(2, hal.tx_trigger_count);
    TEST_ASSERT_TRUE(radio.lastTxSawIrq());
    TEST_ASSERT_EQUAL_UINT8(0, static_cast<uint8_t>(hal.regs[0x07] & (1 << 4)));
}

void test_transmitOnce_without_irq_wire_still_uses_status_polling(void)
{
    FakeHal hal;
    hal.irq_connected = false;
    hal.next_tx_success = true;

    Nrf24 radio(hal);
    const uint8_t payload[] = {0x55};

    const bool ok = radio.transmitOnce(payload, sizeof(payload), 1000);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_FALSE(radio.lastTxSawIrq());
}

void test_transmitOnce_rearm_preserves_runtime_power_level(void)
{
    FakeHal hal;
    hal.next_tx_success = false;

    Nrf24 radio(hal);
    const uint8_t payload[] = {0xAA};

    TEST_ASSERT_TRUE(radio.setRfPowerLevel(1));
    TEST_ASSERT_FALSE(radio.transmitOnce(payload, sizeof(payload), 1000));
    TEST_ASSERT_EQUAL_UINT8(0x02, static_cast<uint8_t>(hal.regs[0x06] & 0x06));
}

void test_readOnePacket_reads_payload_and_clears_rx_flag(void)
{
    FakeHal hal;
    Nrf24 radio(hal);
    radio.setStaticPayloadSize(4);
    hal.loadRxPayload({0x10, 0x20, 0x30, 0x40});

    uint8_t out[4] = {};
    size_t out_len = 0;

    const bool ok = radio.readOnePacket(out, sizeof(out), out_len);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_UINT32(4, static_cast<uint32_t>(out_len));
    TEST_ASSERT_EQUAL_UINT8(0x10, out[0]);
    TEST_ASSERT_EQUAL_UINT8(0x20, out[1]);
    TEST_ASSERT_EQUAL_UINT8(0x30, out[2]);
    TEST_ASSERT_EQUAL_UINT8(0x40, out[3]);
    TEST_ASSERT_TRUE(hal.rx_fifo_packets.empty());
    TEST_ASSERT_EQUAL_UINT8(0, static_cast<uint8_t>(hal.regs[0x07] & (1 << 6)));
}

void test_readOnePacket_preserves_following_rx_payloads(void)
{
    FakeHal hal;
    Nrf24 radio(hal);
    radio.setStaticPayloadSize(4);
    hal.queueRxPayload({0x10, 0x20, 0x30, 0x40});
    hal.queueRxPayload({0x50, 0x60, 0x70, 0x80});

    uint8_t out[4] = {};
    size_t out_len = 0;

    TEST_ASSERT_TRUE(radio.readOnePacket(out, sizeof(out), out_len));
    TEST_ASSERT_EQUAL_UINT32(4, static_cast<uint32_t>(out_len));
    TEST_ASSERT_EQUAL_UINT8(0x10, out[0]);
    TEST_ASSERT_EQUAL_UINT8(0x20, out[1]);
    TEST_ASSERT_EQUAL_UINT8(0x30, out[2]);
    TEST_ASSERT_EQUAL_UINT8(0x40, out[3]);
    TEST_ASSERT_EQUAL(1, static_cast<int>(hal.rx_fifo_packets.size()));
    TEST_ASSERT_EQUAL_UINT8(0, static_cast<uint8_t>(hal.regs[0x17] & (1 << 0)));

    TEST_ASSERT_TRUE(radio.readOnePacket(out, sizeof(out), out_len));
    TEST_ASSERT_EQUAL_UINT8(0x50, out[0]);
    TEST_ASSERT_EQUAL_UINT8(0x60, out[1]);
    TEST_ASSERT_EQUAL_UINT8(0x70, out[2]);
    TEST_ASSERT_EQUAL_UINT8(0x80, out[3]);
    TEST_ASSERT_TRUE(hal.rx_fifo_packets.empty());
    TEST_ASSERT_TRUE((hal.regs[0x17] & (1 << 0)) != 0);
}

void test_radioManager_boot_success_transitions_to_standby(void)
{
    FakeHal hal;
    hal.regs[0x05] = 76;
    Nrf24 radio(hal);
    RadioManager manager(radio);

    const bool ok = manager.boot(42);
    const RadioStatus status = manager.status();

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL(static_cast<int>(RadioState::Standby), static_cast<int>(status.state));
    TEST_ASSERT_EQUAL_UINT8(42, status.channel);
    TEST_ASSERT_EQUAL_INT(3, status.power_level);
    TEST_ASSERT_EQUAL_INT(0, status.last_fault);
}

void test_radioManager_boot_invalid_channel_sets_fault_code(void)
{
    FakeHal hal;
    hal.regs[0x05] = 76;
    Nrf24 radio(hal);
    RadioManager manager(radio);

    const bool ok = manager.boot(126);
    const RadioStatus status = manager.status();

    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_EQUAL(static_cast<int>(RadioState::Fault), static_cast<int>(status.state));
    TEST_ASSERT_EQUAL_INT(2, status.last_fault);
}

void test_radioManager_setPowerLevel_persists_across_reboot(void)
{
    FakeHal hal;
    hal.regs[0x05] = 76;
    Nrf24 radio(hal);
    RadioManager manager(radio);

    TEST_ASSERT_TRUE(manager.boot(76));
    TEST_ASSERT_TRUE(manager.setPowerLevel(1));
    TEST_ASSERT_TRUE(manager.boot(42));

    const RadioStatus status = manager.status();
    TEST_ASSERT_EQUAL(static_cast<int>(RadioState::Standby), static_cast<int>(status.state));
    TEST_ASSERT_EQUAL_UINT8(42, status.channel);
    TEST_ASSERT_EQUAL_INT(1, status.power_level);
    TEST_ASSERT_EQUAL_UINT8(0x02, static_cast<uint8_t>(hal.regs[0x06] & 0x06));
}

void test_radioManager_sendPayload_success_updates_status(void)
{
    FakeHal hal;
    hal.next_tx_success = true;
    Nrf24 radio(hal);
    RadioManager manager(radio);
    const uint8_t payload[] = {0x01, 0x02, 0x03};

    const bool ok = manager.sendPayload(payload, sizeof(payload));
    const RadioStatus status = manager.status();

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_TRUE(status.irq_connected);
    TEST_ASSERT_FALSE(status.irq_asserted);
    TEST_ASSERT_TRUE(status.last_tx_saw_irq);
    TEST_ASSERT_TRUE(status.last_tx_ok);
    TEST_ASSERT_EQUAL(static_cast<int>(RadioState::Standby), static_cast<int>(status.state));
}

void test_radioManager_refreshSnapshot_reports_live_irq_state(void)
{
    FakeHal hal;
    hal.regs[0x07] |= (1 << 6);
    Nrf24 radio(hal);
    RadioManager manager(radio);

    manager.refreshSnapshot();
    const RadioStatus status = manager.status();

    TEST_ASSERT_TRUE(status.irq_connected);
    TEST_ASSERT_TRUE(status.irq_asserted);
}

void test_radioManager_receivePayload_updates_rx_length(void)
{
    FakeHal hal;
    Nrf24 radio(hal);
    radio.setStaticPayloadSize(4);
    RadioManager manager(radio);
    hal.loadRxPayload({0x21, 0x22, 0x23, 0x24});

    uint8_t out[4] = {};
    size_t out_len = 0;

    const bool ok = manager.receivePayload(out, sizeof(out), out_len);
    const RadioStatus status = manager.status();

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_UINT32(4, static_cast<uint32_t>(out_len));
    TEST_ASSERT_EQUAL_UINT32(4, static_cast<uint32_t>(status.last_rx_len));
    TEST_ASSERT_EQUAL(static_cast<int>(RadioState::RxListening), static_cast<int>(status.state));
}

void test_radioManager_hasPendingRx_reports_fifo_backlog_after_irq_clear(void)
{
    FakeHal hal;
    Nrf24 radio(hal);
    radio.setStaticPayloadSize(4);
    RadioManager manager(radio);
    hal.queueRxPayload({0x21, 0x22, 0x23, 0x24});
    hal.queueRxPayload({0x31, 0x32, 0x33, 0x34});

    uint8_t out[4] = {};
    size_t out_len = 0;

    TEST_ASSERT_TRUE(manager.receivePayload(out, sizeof(out), out_len));
    TEST_ASSERT_EQUAL_UINT8(0, static_cast<uint8_t>(hal.regs[0x07] & (1 << 6)));
    TEST_ASSERT_TRUE(manager.hasPendingRx());
}

void test_radioManager_hasPendingRx_true_when_rx_dr_set(void)
{
    FakeHal hal;
    Nrf24 radio(hal);
    RadioManager manager(radio);

    hal.regs[0x07] |= (1 << 6);
    hal.regs[0x17] |= 0x01;

    TEST_ASSERT_TRUE(manager.hasPendingRx());
}

void test_radioManager_hasPendingRx_true_when_fifo_not_empty_without_rx_dr(void)
{
    FakeHal hal;
    Nrf24 radio(hal);
    RadioManager manager(radio);

    hal.regs[0x07] &= static_cast<uint8_t>(~(1 << 6));
    hal.regs[0x17] &= static_cast<uint8_t>(~0x01);

    TEST_ASSERT_TRUE(manager.hasPendingRx());
}

void test_radioManager_hasPendingRx_false_when_rx_dr_clear_and_fifo_empty(void)
{
    FakeHal hal;
    Nrf24 radio(hal);
    RadioManager manager(radio);

    hal.regs[0x07] &= static_cast<uint8_t>(~(1 << 6));
    hal.regs[0x17] |= 0x01;

    TEST_ASSERT_FALSE(manager.hasPendingRx());
}

void test_readOnePacket_does_not_flush_rx_after_successful_read(void)
{
    FakeHal hal;
    Nrf24 radio(hal);
    radio.setStaticPayloadSize(4);
    hal.loadRxPayload({0x10, 0x20, 0x30, 0x40});

    uint8_t out[4] = {};
    size_t out_len = 0;

    TEST_ASSERT_TRUE(radio.readOnePacket(out, sizeof(out), out_len));
    TEST_ASSERT_EQUAL(0, hal.flush_rx_count);
}

void test_radioManager_sends_encoded_audio_as_fixed_width_zero_padded_payload(void)
{
    FakeHal hal;
    hal.next_tx_success = true;
    Nrf24 radio(hal);
    RadioManager manager(radio);

    const uint8_t audio[] = {0x11, 0x22, 0x33};
    uint8_t packet[AudioPacket::kPacketBytes];
    for (size_t i = 0; i < AudioPacket::kPacketBytes; ++i) {
        packet[i] = 0xA5;
    }
    size_t packet_len = 0;

    TEST_ASSERT_TRUE(AudioPacket::encode(5, audio, sizeof(audio), true, false, packet, packet_len));
    TEST_ASSERT_EQUAL_UINT32(AudioPacket::kPacketBytes, static_cast<uint32_t>(packet_len));
    TEST_ASSERT_TRUE(manager.sendPayload(packet, AudioPacket::kPacketBytes));

    TEST_ASSERT_EQUAL_UINT32(AudioPacket::kPacketBytes, static_cast<uint32_t>(hal.last_payload_write.size()));
    for (size_t i = AudioPacket::kHeaderBytes + sizeof(audio); i < AudioPacket::kPacketBytes; ++i) {
        TEST_ASSERT_EQUAL_UINT8(0, hal.last_payload_write[i]);
    }
}

void test_radioManager_startCw_updates_output_power(void)
{
    FakeHal hal;
    Nrf24 radio(hal);
    RadioManager manager(radio);

    TEST_ASSERT_TRUE(manager.startCw(76, 0x02));

    const RadioStatus status = manager.status();
    TEST_ASSERT_EQUAL(static_cast<int>(RadioState::CwTest), static_cast<int>(status.state));
    TEST_ASSERT_EQUAL_UINT8(76, status.channel);
    TEST_ASSERT_EQUAL_INT(1, status.power_level);
}

// Utility tests
void test_txHelpers_pacing_8_bytes_delays_1_ms_with_no_remainder(void)
{
    const TxHelpers::PacingDelay delay = TxHelpers::calculateAudioPacingDelay(8, 0);

    TEST_ASSERT_EQUAL_UINT32(1, delay.delay_ms);
    TEST_ASSERT_EQUAL_UINT32(0, delay.remainder_us);
}

void test_txHelpers_pacing_single_bytes_accumulate_to_1_ms(void)
{
    uint32_t remainder_us = 0;
    uint32_t total_delay_ms = 0;

    for (int i = 0; i < 8; ++i) {
        const TxHelpers::PacingDelay delay =
            TxHelpers::calculateAudioPacingDelay(1, remainder_us);
        total_delay_ms += delay.delay_ms;
        remainder_us = delay.remainder_us;
    }

    TEST_ASSERT_EQUAL_UINT32(1, total_delay_ms);
    TEST_ASSERT_EQUAL_UINT32(0, remainder_us);
}

void test_txHelpers_pacing_10_bytes_keeps_250_us_remainder(void)
{
    const TxHelpers::PacingDelay delay = TxHelpers::calculateAudioPacingDelay(10, 0);

    TEST_ASSERT_EQUAL_UINT32(1, delay.delay_ms);
    TEST_ASSERT_EQUAL_UINT32(250, delay.remainder_us);
}

void test_txHelpers_retry_succeeds_on_first_try(void)
{
    int send_calls = 0;
    int delay_calls = 0;

    const bool ok = TxHelpers::sendWithRetry(
        [&]() {
            ++send_calls;
            return true;
        },
        [&](uint32_t) {
            ++delay_calls;
        });

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL(1, send_calls);
    TEST_ASSERT_EQUAL(0, delay_calls);
}

void test_txHelpers_retry_succeeds_on_retry(void)
{
    int send_calls = 0;
    uint32_t delayed_ms = 0;

    const bool ok = TxHelpers::sendWithRetry(
        [&]() {
            ++send_calls;
            return send_calls == 2;
        },
        [&](uint32_t ms) {
            delayed_ms += ms;
        });

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL(2, send_calls);
    TEST_ASSERT_EQUAL_UINT32(TxHelpers::kDefaultRetryDelayMs, delayed_ms);
}

void test_txHelpers_retry_fails_after_all_attempts(void)
{
    int send_calls = 0;
    int delay_calls = 0;

    const bool ok = TxHelpers::sendWithRetry(
        [&]() {
            ++send_calls;
            return false;
        },
        [&](uint32_t) {
            ++delay_calls;
        });

    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_EQUAL(TxHelpers::kDefaultSendAttempts, send_calls);
    TEST_ASSERT_EQUAL(TxHelpers::kDefaultSendAttempts - 1, delay_calls);
}

void test_rxDrain_processes_multiple_pending_packets(void)
{
    int pending = 3;
    int processed = 0;

    const RxDrain::DrainResult result = RxDrain::drainPending(
        [&]() {
            return pending > 0;
        },
        [&]() {
            --pending;
            ++processed;
            return RxDrain::StepResult::Processed;
        });

    TEST_ASSERT_EQUAL_UINT32(3, static_cast<uint32_t>(result.processed));
    TEST_ASSERT_EQUAL(3, processed);
    TEST_ASSERT_FALSE(result.receive_failed);
}

void test_rxDrain_exits_on_receive_failure(void)
{
    int step_calls = 0;

    const RxDrain::DrainResult result = RxDrain::drainPending(
        []() {
            return true;
        },
        [&]() {
            ++step_calls;
            return step_calls == 1 ? RxDrain::StepResult::Processed : RxDrain::StepResult::Failed;
        },
        8);

    TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(result.processed));
    TEST_ASSERT_EQUAL(2, step_calls);
    TEST_ASSERT_TRUE(result.receive_failed);
}

void test_frame_io_round_trip_preserves_record(void)
{
    FrameRecord original;
    original.is_tx = true;
    original.timestamp_us = 123456;
    original.channel = 76;
    original.payload = {0xA1, 0xB2, 0x03};

    const std::string line = FrameIO::toLine(original);

    FrameRecord parsed;
    const bool ok = FrameIO::fromLine(line, parsed);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_TRUE(parsed.is_tx);
    TEST_ASSERT_EQUAL_UINT32(123456, static_cast<uint32_t>(parsed.timestamp_us));
    TEST_ASSERT_EQUAL_UINT8(76, parsed.channel);
    assertBytes(parsed.payload, {0xA1, 0xB2, 0x03});
}

void test_validation_rejects_oversized_payload(void)
{
    const ValidationResult result = Validation::payloadSize(33);

    TEST_ASSERT_FALSE(result.ok);
    TEST_ASSERT_EQUAL_STRING("payload exceeds 32 bytes", result.message);
}

void test_morse_encode_e_creates_single_dot_event(void)
{
    const std::vector<KeyEvent> events = Morse::encode("E", 100);

    TEST_ASSERT_EQUAL(1, static_cast<int>(events.size()));
    TEST_ASSERT_TRUE(events[0].key_down);
    TEST_ASSERT_EQUAL_UINT32(100, events[0].duration_ms);
}

void test_morse_encode_word_gap_is_seven_dots(void)
{
    const std::vector<KeyEvent> events = Morse::encode("E E", 50);

    TEST_ASSERT_EQUAL(3, static_cast<int>(events.size()));
    TEST_ASSERT_TRUE(events[0].key_down);
    TEST_ASSERT_FALSE(events[1].key_down);
    TEST_ASSERT_TRUE(events[2].key_down);
    TEST_ASSERT_EQUAL_UINT32(50, events[0].duration_ms);
    TEST_ASSERT_EQUAL_UINT32(350, events[1].duration_ms);
    TEST_ASSERT_EQUAL_UINT32(50, events[2].duration_ms);
}

void test_morse_render_formats_letters_and_words_on_one_line(void)
{
    TEST_ASSERT_EQUAL_STRING("... --- ... / .---- ..--- ...--", Morse::render("SOS 123").c_str());
}

void test_stopContinuousCarrier_restores_demo_rf_setup(void)
{
    FakeHal hal;
    Nrf24 radio(hal);

    TEST_ASSERT_TRUE(radio.initDefaults(76));
    TEST_ASSERT_TRUE(radio.startContinuousCarrier(76, 0x06));

    radio.stopContinuousCarrier();

    TEST_ASSERT_FALSE(hal.ce_level);
    TEST_ASSERT_EQUAL_UINT8(0x26, hal.regs[0x06]);
    TEST_ASSERT_TRUE((hal.regs[0x00] & (1 << 1)) != 0);
}

void test_startContinuousCarrier_uses_cont_wave_when_supported(void)
{
    FakeHal hal;
    Nrf24 radio(hal);

    TEST_ASSERT_TRUE(radio.initDefaults(76));
    TEST_ASSERT_TRUE(radio.startContinuousCarrier(40, 0x06));

    TEST_ASSERT_TRUE(hal.ce_level);
    TEST_ASSERT_EQUAL_UINT8(40, hal.regs[0x05]);
    TEST_ASSERT_EQUAL_UINT8(0xB6, hal.regs[0x06]);
    TEST_ASSERT_TRUE(hal.last_payload_write.empty());
}

void test_startContinuousCarrier_falls_back_to_payload_reuse_when_needed(void)
{
    FakeHal hal;
    hal.supports_cont_wave = false;

    Nrf24 radio(hal);

    TEST_ASSERT_TRUE(radio.initDefaults(76));

    const uint8_t saved_rf_setup = hal.regs[0x06];
    const uint8_t saved_tx_addr_0 = hal.regs[0x10];
    const uint8_t saved_tx_addr_1 = hal.regs[0x11];
    const uint8_t saved_tx_addr_2 = hal.regs[0x12];
    const uint8_t saved_tx_addr_3 = hal.regs[0x13];
    const uint8_t saved_tx_addr_4 = hal.regs[0x14];

    TEST_ASSERT_TRUE(radio.startContinuousCarrier(33, 0x04));

    TEST_ASSERT_TRUE(hal.ce_level);
    TEST_ASSERT_EQUAL_UINT8(33, hal.regs[0x05]);
    TEST_ASSERT_EQUAL(32, static_cast<int>(hal.last_payload_write.size()));
    TEST_ASSERT_EQUAL(2, hal.tx_trigger_count);
    TEST_ASSERT_TRUE(hal.tx_reuse);
    TEST_ASSERT_EQUAL_UINT8(0xE3, hal.last_tx[0]);
    TEST_ASSERT_EQUAL_UINT8(0xFF, hal.regs[0x10]);
    TEST_ASSERT_EQUAL_UINT8(0xFF, hal.regs[0x11]);
    TEST_ASSERT_EQUAL_UINT8(0xFF, hal.regs[0x12]);
    TEST_ASSERT_EQUAL_UINT8(0xFF, hal.regs[0x13]);
    TEST_ASSERT_EQUAL_UINT8(0xFF, hal.regs[0x14]);
    TEST_ASSERT_EQUAL_UINT8(0x00, static_cast<uint8_t>(hal.regs[0x00] & 0x0C));

    radio.stopContinuousCarrier();

    TEST_ASSERT_FALSE(hal.ce_level);
    TEST_ASSERT_EQUAL_UINT8(saved_rf_setup, hal.regs[0x06]);
    TEST_ASSERT_EQUAL_UINT8(saved_tx_addr_0, hal.regs[0x10]);
    TEST_ASSERT_EQUAL_UINT8(saved_tx_addr_1, hal.regs[0x11]);
    TEST_ASSERT_EQUAL_UINT8(saved_tx_addr_2, hal.regs[0x12]);
    TEST_ASSERT_EQUAL_UINT8(saved_tx_addr_3, hal.regs[0x13]);
    TEST_ASSERT_EQUAL_UINT8(saved_tx_addr_4, hal.regs[0x14]);
}

void test_audioPacket_decode_accepts_full_width_padded_packet(void)
{
    const uint8_t audio[] = {0x12, 0x34, 0x56};
    uint8_t packet[AudioPacket::kPacketBytes] = {};
    size_t packet_len = 0;

    TEST_ASSERT_TRUE(AudioPacket::encode(7, audio, sizeof(audio), true, false, packet, packet_len));

    // In the current static-payload design, encode() is allowed to return
    // a full fixed-width packet.
    TEST_ASSERT_EQUAL_UINT32(AudioPacket::kPacketBytes, static_cast<uint32_t>(packet_len));

    AudioPacket::Header header;
    const uint8_t* decoded_audio = nullptr;

    TEST_ASSERT_TRUE(AudioPacket::decode(packet, packet_len, header, decoded_audio));
    TEST_ASSERT_EQUAL_UINT16(7, header.sequence);
    TEST_ASSERT_EQUAL_UINT8(sizeof(audio), header.audio_len);
    TEST_ASSERT_EQUAL_UINT8(AudioPacket::kFirst, header.flags);
    assertBytes(std::vector<uint8_t>(decoded_audio, decoded_audio + header.audio_len), {0x12, 0x34, 0x56});
}

void test_audioPacket_decode_accepts_exact_length_packet(void)
{
    const uint8_t audio[] = {0xAA, 0xBB, 0xCC};
    uint8_t packet[AudioPacket::kPacketBytes] = {};
    size_t packet_len = 0;

    TEST_ASSERT_TRUE(AudioPacket::encode(3, audio, sizeof(audio), false, false, packet, packet_len));

    AudioPacket::Header header;
    const uint8_t* decoded_audio = nullptr;

    TEST_ASSERT_TRUE(AudioPacket::decode(packet,
                                         AudioPacket::kHeaderBytes + sizeof(audio),
                                         header,
                                         decoded_audio));
    TEST_ASSERT_EQUAL_UINT16(3, header.sequence);
    TEST_ASSERT_EQUAL_UINT8(sizeof(audio), header.audio_len);
    assertBytes(std::vector<uint8_t>(decoded_audio, decoded_audio + header.audio_len), {0xAA, 0xBB, 0xCC});
}

void test_audioPacket_decode_accepts_partially_padded_packet(void)
{
    const uint8_t audio[] = {0xAA, 0xBB, 0xCC};
    uint8_t packet[AudioPacket::kPacketBytes] = {};
    size_t packet_len = 0;

    TEST_ASSERT_TRUE(AudioPacket::encode(3, audio, sizeof(audio), false, false, packet, packet_len));

    AudioPacket::Header header;
    const uint8_t* decoded_audio = nullptr;

    TEST_ASSERT_TRUE(AudioPacket::decode(packet, 9, header, decoded_audio));
    TEST_ASSERT_EQUAL_UINT16(3, header.sequence);
    TEST_ASSERT_EQUAL_UINT8(sizeof(audio), header.audio_len);
}

void test_audioPacket_decode_rejects_too_short_packet(void)
{
    const uint8_t audio[] = {0xAA, 0xBB, 0xCC};
    uint8_t packet[AudioPacket::kPacketBytes] = {};
    size_t packet_len = 0;

    TEST_ASSERT_TRUE(AudioPacket::encode(3, audio, sizeof(audio), false, false, packet, packet_len));

    AudioPacket::Header header;
    const uint8_t* decoded_audio = nullptr;

    TEST_ASSERT_FALSE(AudioPacket::decode(packet,
                                          AudioPacket::kHeaderBytes + sizeof(audio) - 1u,
                                          header,
                                          decoded_audio));
}

void test_audioPacket_decode_rejects_oversized_packet(void)
{
    const uint8_t audio[] = {0xAA};
    uint8_t packet[AudioPacket::kPacketBytes + 1] = {};
    size_t packet_len = 0;

    TEST_ASSERT_TRUE(AudioPacket::encode(3, audio, sizeof(audio), false, false, packet, packet_len));

    AudioPacket::Header header;
    const uint8_t* decoded_audio = nullptr;

    TEST_ASSERT_FALSE(AudioPacket::decode(packet, sizeof(packet), header, decoded_audio));
}

void test_audioPacket_decode_rejects_declared_audio_len_larger_than_max(void)
{
    uint8_t packet[AudioPacket::kPacketBytes] = {};
    packet[2] = static_cast<uint8_t>(AudioPacket::kAudioBytesPerPacket + 1u);

    AudioPacket::Header header;
    const uint8_t* decoded_audio = nullptr;

    TEST_ASSERT_FALSE(AudioPacket::decode(packet, AudioPacket::kPacketBytes, header, decoded_audio));
}

void test_readOnePacket_reads_fifo_backlog_even_if_rx_dr_is_clear(void)
{
    FakeHal hal;
    Nrf24 radio(hal);
    radio.setStaticPayloadSize(4);

    hal.loadRxPayload({0x41, 0x42, 0x43, 0x44});
    hal.regs[0x07] &= static_cast<uint8_t>(~(1 << 6)); // clear RX_DR
    hal.regs[0x17] = 0x00;                              // RX_EMPTY clear => data pending

    uint8_t out[4] = {};
    size_t out_len = 0;

    const bool ok = radio.readOnePacket(out, sizeof(out), out_len);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_UINT32(4, static_cast<uint32_t>(out_len));
    TEST_ASSERT_EQUAL_UINT8(0x41, out[0]);
    TEST_ASSERT_EQUAL_UINT8(0x42, out[1]);
    TEST_ASSERT_EQUAL_UINT8(0x43, out[2]);
    TEST_ASSERT_EQUAL_UINT8(0x44, out[3]);
    TEST_ASSERT_EQUAL_UINT8(0, static_cast<uint8_t>(hal.regs[0x07] & (1 << 6)));
}

void test_radioManager_hasPendingRx_reports_fifo_backlog_even_after_irq_clear(void)
{
    FakeHal hal;
    Nrf24 radio(hal);
    RadioManager manager(radio);

    hal.regs[0x07] &= static_cast<uint8_t>(~(1 << 6));
    hal.regs[0x17] = 0x00;

    TEST_ASSERT_TRUE(manager.hasPendingRx());

    const RadioStatus status = manager.status();
    TEST_ASSERT_EQUAL_UINT8(0x00, static_cast<uint8_t>(status.last_fifo_status & 0x01));
}

void test_radioManager_receivePayload_reads_fifo_backlog_after_irq_clear(void)
{
    FakeHal hal;
    Nrf24 radio(hal);
    radio.setStaticPayloadSize(4);
    RadioManager manager(radio);

    hal.loadRxPayload({0x21, 0x22, 0x23, 0x24});
    hal.regs[0x07] &= static_cast<uint8_t>(~(1 << 6)); // clear RX_DR
    hal.regs[0x17] = 0x00;                              // RX_EMPTY clear => data pending

    uint8_t out[4] = {};
    size_t out_len = 0;

    const bool ok = manager.receivePayload(out, sizeof(out), out_len);
    const RadioStatus status = manager.status();

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_UINT32(4, static_cast<uint32_t>(out_len));
    TEST_ASSERT_EQUAL_UINT32(4, static_cast<uint32_t>(status.last_rx_len));
    TEST_ASSERT_EQUAL_UINT8(0x21, out[0]);
    TEST_ASSERT_EQUAL_UINT8(0x22, out[1]);
    TEST_ASSERT_EQUAL_UINT8(0x23, out[2]);
    TEST_ASSERT_EQUAL_UINT8(0x24, out[3]);
    TEST_ASSERT_EQUAL(static_cast<int>(RadioState::RxListening), static_cast<int>(status.state));
}

void test_audioPacket_decode_accepts_every_valid_audio_len_as_padded_packet(void)
{
    for (size_t len = 1; len <= AudioPacket::kAudioBytesPerPacket; ++len) {
        std::vector<uint8_t> audio(len);
        for (size_t i = 0; i < len; ++i) {
            audio[i] = static_cast<uint8_t>(i + 1);
        }

        uint8_t packet[AudioPacket::kPacketBytes] = {};
        size_t packet_len = 0;

        TEST_ASSERT_TRUE(AudioPacket::encode(42,
                                             audio.data(),
                                             audio.size(),
                                             len == 1,
                                             len == AudioPacket::kAudioBytesPerPacket,
                                             packet,
                                             packet_len));
        TEST_ASSERT_EQUAL_UINT32(AudioPacket::kPacketBytes, static_cast<uint32_t>(packet_len));

        AudioPacket::Header header;
        const uint8_t* decoded_audio = nullptr;

        TEST_ASSERT_TRUE(AudioPacket::decode(packet, packet_len, header, decoded_audio));
        TEST_ASSERT_EQUAL_UINT16(42, header.sequence);
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(len), header.audio_len);

        for (size_t i = 0; i < len; ++i) {
            TEST_ASSERT_EQUAL_UINT8(audio[i], decoded_audio[i]);
        }
    }
}

void test_audioPacket_decode_accepts_zero_audio_len(void)
{
    uint8_t packet[AudioPacket::kPacketBytes] = {};
    packet[0] = 0x01;
    packet[1] = 0x00;
    packet[2] = 0x00; // audio_len = 0
    packet[3] = 0x00;

    AudioPacket::Header header;
    const uint8_t* decoded_audio = nullptr;

    TEST_ASSERT_TRUE(AudioPacket::decode(packet, AudioPacket::kPacketBytes, header, decoded_audio));
    TEST_ASSERT_EQUAL_UINT16(1, header.sequence);
    TEST_ASSERT_EQUAL_UINT8(0, header.audio_len);
    TEST_ASSERT_EQUAL_PTR(packet + AudioPacket::kHeaderBytes, decoded_audio);
}

void test_audioReassembler_accepts_padded_packets_in_order(void)
{
    uint8_t packet0[AudioPacket::kPacketBytes] = {};
    uint8_t packet1[AudioPacket::kPacketBytes] = {};
    size_t len0 = 0;
    size_t len1 = 0;

    TEST_ASSERT_TRUE(AudioPacket::encode(0,
                                         reinterpret_cast<const uint8_t*>("abc"),
                                         3,
                                         true,
                                         false,
                                         packet0,
                                         len0));
    TEST_ASSERT_TRUE(AudioPacket::encode(1,
                                         reinterpret_cast<const uint8_t*>("de"),
                                         2,
                                         false,
                                         true,
                                         packet1,
                                         len1));

    TEST_ASSERT_EQUAL_UINT32(AudioPacket::kPacketBytes, static_cast<uint32_t>(len0));
    TEST_ASSERT_EQUAL_UINT32(AudioPacket::kPacketBytes, static_cast<uint32_t>(len1));

    AudioReassembler reassembler;
    TEST_ASSERT_TRUE(reassembler.acceptPacket(packet0, len0));
    TEST_ASSERT_TRUE(reassembler.acceptPacket(packet1, len1));

    TEST_ASSERT_TRUE(reassembler.started());
    TEST_ASSERT_TRUE(reassembler.complete());
    TEST_ASSERT_EQUAL_UINT16(2, reassembler.nextSequence());
    assertBytes(reassembler.audio(), {'a', 'b', 'c', 'd', 'e'});
}

void test_readOnePacket_returns_false_when_no_pending_data(void)
{
    FakeHal hal;
    Nrf24 radio(hal);
    radio.setStaticPayloadSize(4);

    hal.regs[0x07] &= static_cast<uint8_t>(~(1 << 6)); // RX_DR clear
    hal.regs[0x17] |= 0x01;                            // RX_EMPTY set

    uint8_t out[4] = {};
    size_t out_len = 0;

    TEST_ASSERT_FALSE(radio.readOnePacket(out, sizeof(out), out_len));
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(out_len));
}

void test_radioManager_receivePayload_without_data_sets_fault(void)
{
    FakeHal hal;
    Nrf24 radio(hal);
    radio.setStaticPayloadSize(4);
    RadioManager manager(radio);

    hal.regs[0x07] &= static_cast<uint8_t>(~(1 << 6)); // RX_DR clear
    hal.regs[0x17] |= 0x01;                            // RX_EMPTY set

    uint8_t out[4] = {};
    size_t out_len = 0;

    TEST_ASSERT_FALSE(manager.receivePayload(out, sizeof(out), out_len));

    const RadioStatus status = manager.status();
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(out_len));
    TEST_ASSERT_EQUAL(static_cast<int>(RadioState::Fault), static_cast<int>(status.state));
    TEST_ASSERT_EQUAL_INT(4, status.last_fault);
}

void test_transmitOnce_timeout_without_irq_returns_false_and_sets_timeout(void)
{
    TimeoutHal hal;
    hal.irq_connected = false;
    hal.now_step_us = 200;

    Nrf24 radio(hal);
    const uint8_t payload[] = {0x11, 0x22, 0x33};

    const bool ok = radio.transmitOnce(payload, sizeof(payload), 1000);

    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_EQUAL(2, hal.tx_trigger_count);
    TEST_ASSERT_TRUE(radio.lastTxTimedOut());
    TEST_ASSERT_FALSE(radio.lastTxSawIrq());
    TEST_ASSERT_TRUE(hal.tx_fifo.empty());
}

void test_radioManager_boot_probe_failure_sets_fault_code_1(void)
{
    ProbeFailHal hal;
    hal.regs[0x05] = 76;

    Nrf24 radio(hal);
    RadioManager manager(radio);

    const bool ok = manager.boot(40);
    const RadioStatus status = manager.status();

    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_EQUAL(static_cast<int>(RadioState::Fault), static_cast<int>(status.state));
    TEST_ASSERT_EQUAL_INT(1, status.last_fault);
}

void test_streamSync_start_round_trip(void)
{
    uint8_t packet[AudioPacket::kPacketBytes] = {};
    size_t packet_len = 0;

    TEST_ASSERT_TRUE(StreamSync::encodeStart(0x1234, packet, packet_len));
    TEST_ASSERT_EQUAL_UINT32(AudioPacket::kPacketBytes, static_cast<uint32_t>(packet_len));

    StreamSync::ControlFrame frame;
    TEST_ASSERT_TRUE(StreamSync::decodeStart(packet, packet_len, frame));
    TEST_ASSERT_EQUAL_UINT16(0x1234, frame.stream_id);
}

void test_streamSync_stop_round_trip(void)
{
    uint8_t packet[AudioPacket::kPacketBytes] = {};
    size_t packet_len = 0;

    TEST_ASSERT_TRUE(StreamSync::encodeStop(0x4321, packet, packet_len));
    TEST_ASSERT_EQUAL_UINT32(AudioPacket::kPacketBytes, static_cast<uint32_t>(packet_len));

    StreamSync::ControlFrame frame;
    TEST_ASSERT_TRUE(StreamSync::decodeStop(packet, packet_len, frame));
    TEST_ASSERT_EQUAL_UINT16(0x4321, frame.stream_id);
}

void test_streamSync_remote_command_round_trip(void)
{
    static constexpr char kCommand[] = "TX LOOP 2 song.u8";
    uint8_t packet[AudioPacket::kPacketBytes] = {};
    size_t packet_len = 0;

    TEST_ASSERT_TRUE(StreamSync::encodeRemoteCommand(kCommand,
                                                     sizeof(kCommand) - 1,
                                                     packet,
                                                     packet_len));
    TEST_ASSERT_EQUAL_UINT32(AudioPacket::kPacketBytes, static_cast<uint32_t>(packet_len));

    std::string_view command;
    TEST_ASSERT_TRUE(StreamSync::decodeRemoteCommand(packet, packet_len, command));
    TEST_ASSERT_EQUAL_UINT32(sizeof(kCommand) - 1, static_cast<uint32_t>(command.size()));
    TEST_ASSERT_EQUAL_MEMORY(kCommand, command.data(), sizeof(kCommand) - 1);
}

void test_streamSync_remote_command_rejects_oversized_text(void)
{
    std::array<char, StreamSync::kRemoteCommandMaxBytes + 2> command{};
    command.fill('A');

    uint8_t packet[AudioPacket::kPacketBytes] = {};
    size_t packet_len = 99;

    TEST_ASSERT_FALSE(StreamSync::encodeRemoteCommand(command.data(),
                                                      command.size(),
                                                      packet,
                                                      packet_len));
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(packet_len));
}

void test_streamSync_remote_response_round_trip(void)
{
    static constexpr char kText[] = "[peer] State=RxListening\n";
    uint8_t packet[AudioPacket::kPacketBytes] = {};
    size_t packet_len = 0;

    TEST_ASSERT_TRUE(StreamSync::encodeRemoteResponse(kText,
                                                      sizeof(kText) - 1,
                                                      packet,
                                                      packet_len));
    TEST_ASSERT_EQUAL_UINT32(AudioPacket::kPacketBytes, static_cast<uint32_t>(packet_len));

    std::string_view text;
    TEST_ASSERT_TRUE(StreamSync::decodeRemoteResponse(packet, packet_len, text));
    TEST_ASSERT_EQUAL_UINT32(sizeof(kText) - 1, static_cast<uint32_t>(text.size()));
    TEST_ASSERT_EQUAL_MEMORY(kText, text.data(), sizeof(kText) - 1);
}

void test_streamSync_gate_ignores_nonzero_audio_before_sync(void)
{
    uint8_t packet[AudioPacket::kPacketBytes] = {};
    size_t packet_len = 0;

    TEST_ASSERT_TRUE(AudioPacket::encode(1,
                                         reinterpret_cast<const uint8_t*>("abc"),
                                         3,
                                         false,
                                         false,
                                         packet,
                                         packet_len));

    StreamSync::ReceiverGate gate;
    AudioPacket::Header header{};
    const uint8_t* audio = nullptr;

    TEST_ASSERT_EQUAL(static_cast<int>(StreamSync::ReceiverGate::Action::Ignore),
                      static_cast<int>(gate.accept(packet, packet_len, &header, &audio)));
    TEST_ASSERT_EQUAL(static_cast<int>(StreamSync::ReceiverGate::State::WaitingForStart),
                      static_cast<int>(gate.state()));
}

void test_streamSync_gate_accepts_legacy_seq0_without_start(void)
{
    uint8_t packet[AudioPacket::kPacketBytes] = {};
    size_t packet_len = 0;

    TEST_ASSERT_TRUE(AudioPacket::encode(0,
                                         reinterpret_cast<const uint8_t*>("abc"),
                                         3,
                                         true,
                                         false,
                                         packet,
                                         packet_len));

    StreamSync::ReceiverGate gate;
    AudioPacket::Header header{};
    const uint8_t* audio = nullptr;

    TEST_ASSERT_EQUAL(static_cast<int>(StreamSync::ReceiverGate::Action::AudioAccepted),
                      static_cast<int>(gate.accept(packet, packet_len, &header, &audio)));
    TEST_ASSERT_EQUAL(static_cast<int>(StreamSync::ReceiverGate::State::Streaming),
                      static_cast<int>(gate.state()));
    TEST_ASSERT_EQUAL_UINT16(0, header.sequence);
    TEST_ASSERT_EQUAL_UINT8(3, header.audio_len);
}

void test_streamSync_gate_accepts_start_then_seq0(void)
{
    uint8_t start_packet[AudioPacket::kPacketBytes] = {};
    size_t start_len = 0;
    TEST_ASSERT_TRUE(StreamSync::encodeStart(7, start_packet, start_len));

    uint8_t audio_packet[AudioPacket::kPacketBytes] = {};
    size_t audio_len = 0;
    TEST_ASSERT_TRUE(AudioPacket::encode(0,
                                         reinterpret_cast<const uint8_t*>("abc"),
                                         3,
                                         true,
                                         false,
                                         audio_packet,
                                         audio_len));

    StreamSync::ReceiverGate gate;
    AudioPacket::Header header{};
    const uint8_t* audio = nullptr;

    TEST_ASSERT_EQUAL(static_cast<int>(StreamSync::ReceiverGate::Action::StartAccepted),
                      static_cast<int>(gate.accept(start_packet, start_len)));
    TEST_ASSERT_EQUAL(static_cast<int>(StreamSync::ReceiverGate::State::WaitingForSeq0),
                      static_cast<int>(gate.state()));
    TEST_ASSERT_EQUAL_UINT16(7, gate.currentStreamId());

    TEST_ASSERT_EQUAL(static_cast<int>(StreamSync::ReceiverGate::Action::AudioAccepted),
                      static_cast<int>(gate.accept(audio_packet, audio_len, &header, &audio)));
    TEST_ASSERT_EQUAL(static_cast<int>(StreamSync::ReceiverGate::State::Streaming),
                      static_cast<int>(gate.state()));
    TEST_ASSERT_EQUAL_UINT16(0, header.sequence);
    TEST_ASSERT_EQUAL_UINT8(3, header.audio_len);
}

void test_streamSync_gate_ignores_packet_one_until_seq0_arrives(void)
{
    uint8_t start_packet[AudioPacket::kPacketBytes] = {};
    size_t start_len = 0;
    TEST_ASSERT_TRUE(StreamSync::encodeStart(8, start_packet, start_len));

    uint8_t packet1[AudioPacket::kPacketBytes] = {};
    size_t packet1_len = 0;
    TEST_ASSERT_TRUE(AudioPacket::encode(1,
                                         reinterpret_cast<const uint8_t*>("def"),
                                         3,
                                         false,
                                         false,
                                         packet1,
                                         packet1_len));

    uint8_t packet0[AudioPacket::kPacketBytes] = {};
    size_t packet0_len = 0;
    TEST_ASSERT_TRUE(AudioPacket::encode(0,
                                         reinterpret_cast<const uint8_t*>("abc"),
                                         3,
                                         true,
                                         false,
                                         packet0,
                                         packet0_len));

    StreamSync::ReceiverGate gate;

    TEST_ASSERT_EQUAL(static_cast<int>(StreamSync::ReceiverGate::Action::StartAccepted),
                      static_cast<int>(gate.accept(start_packet, start_len)));

    TEST_ASSERT_EQUAL(static_cast<int>(StreamSync::ReceiverGate::Action::Ignore),
                      static_cast<int>(gate.accept(packet1, packet1_len)));
    TEST_ASSERT_EQUAL(static_cast<int>(StreamSync::ReceiverGate::State::WaitingForSeq0),
                      static_cast<int>(gate.state()));

    TEST_ASSERT_EQUAL(static_cast<int>(StreamSync::ReceiverGate::Action::AudioAccepted),
                      static_cast<int>(gate.accept(packet0, packet0_len)));
    TEST_ASSERT_EQUAL(static_cast<int>(StreamSync::ReceiverGate::State::Streaming),
                      static_cast<int>(gate.state()));
}

void test_streamSync_gate_ignores_duplicate_seq0_after_stream_starts(void)
{
    uint8_t start_packet[AudioPacket::kPacketBytes] = {};
    size_t start_len = 0;
    TEST_ASSERT_TRUE(StreamSync::encodeStart(9, start_packet, start_len));

    uint8_t packet0[AudioPacket::kPacketBytes] = {};
    size_t packet0_len = 0;
    TEST_ASSERT_TRUE(AudioPacket::encode(0,
                                         reinterpret_cast<const uint8_t*>("abc"),
                                         3,
                                         true,
                                         false,
                                         packet0,
                                         packet0_len));

    StreamSync::ReceiverGate gate;
    TEST_ASSERT_EQUAL(static_cast<int>(StreamSync::ReceiverGate::Action::StartAccepted),
                      static_cast<int>(gate.accept(start_packet, start_len)));

    TEST_ASSERT_EQUAL(static_cast<int>(StreamSync::ReceiverGate::Action::AudioAccepted),
                      static_cast<int>(gate.accept(packet0, packet0_len)));
    TEST_ASSERT_EQUAL(static_cast<int>(StreamSync::ReceiverGate::Action::Ignore),
                      static_cast<int>(gate.accept(packet0, packet0_len)));
    TEST_ASSERT_EQUAL(static_cast<int>(StreamSync::ReceiverGate::State::Streaming),
                      static_cast<int>(gate.state()));
}

void test_streamSync_gate_new_start_resets_current_stream(void)
{
    uint8_t start_a[AudioPacket::kPacketBytes] = {};
    size_t start_a_len = 0;
    TEST_ASSERT_TRUE(StreamSync::encodeStart(10, start_a, start_a_len));

    uint8_t packet0[AudioPacket::kPacketBytes] = {};
    size_t packet0_len = 0;
    TEST_ASSERT_TRUE(AudioPacket::encode(0,
                                         reinterpret_cast<const uint8_t*>("abc"),
                                         3,
                                         true,
                                         false,
                                         packet0,
                                         packet0_len));
    uint8_t start_b[AudioPacket::kPacketBytes] = {};
    size_t start_b_len = 0;
    TEST_ASSERT_TRUE(StreamSync::encodeStart(11, start_b, start_b_len));

    uint8_t packet1[AudioPacket::kPacketBytes] = {};
    size_t packet1_len = 0;
    TEST_ASSERT_TRUE(AudioPacket::encode(1,
                                         reinterpret_cast<const uint8_t*>("def"),
                                         3,
                                         false,
                                         false,
                                         packet1,
                                         packet1_len));

    StreamSync::ReceiverGate gate;
     TEST_ASSERT_EQUAL(static_cast<int>(StreamSync::ReceiverGate::Action::StartAccepted),
                      static_cast<int>(gate.accept(start_a, start_a_len)));
    TEST_ASSERT_EQUAL(static_cast<int>(StreamSync::ReceiverGate::Action::AudioAccepted),
                      static_cast<int>(gate.accept(packet0, packet0_len)));
    TEST_ASSERT_EQUAL_UINT16(10, gate.currentStreamId());
    TEST_ASSERT_EQUAL(static_cast<int>(StreamSync::ReceiverGate::Action::StartAccepted),
                      static_cast<int>(gate.accept(start_b, start_b_len)));
    TEST_ASSERT_EQUAL_UINT16(11, gate.currentStreamId());
    TEST_ASSERT_EQUAL(static_cast<int>(StreamSync::ReceiverGate::State::WaitingForSeq0),
                      static_cast<int>(gate.state()));

    TEST_ASSERT_EQUAL(static_cast<int>(StreamSync::ReceiverGate::Action::Ignore),
                      static_cast<int>(gate.accept(packet1, packet1_len)));
}

void test_streamSync_gate_stop_returns_to_waiting_for_start(void)
{
    uint8_t start_packet[AudioPacket::kPacketBytes] = {};
    size_t start_len = 0;
    TEST_ASSERT_TRUE(StreamSync::encodeStart(12, start_packet, start_len));

    uint8_t packet0[AudioPacket::kPacketBytes] = {};
    size_t packet0_len = 0;
    TEST_ASSERT_TRUE(AudioPacket::encode(0,
                                         reinterpret_cast<const uint8_t*>("abc"),
                                         3,
                                         true,
                                         false,
                                         packet0,
                                         packet0_len));
    uint8_t stop_packet[AudioPacket::kPacketBytes] = {};
    size_t stop_len = 0;
    TEST_ASSERT_TRUE(StreamSync::encodeStop(12, stop_packet, stop_len));

    StreamSync::ReceiverGate gate;
    TEST_ASSERT_EQUAL(static_cast<int>(StreamSync::ReceiverGate::Action::StartAccepted),
                      static_cast<int>(gate.accept(start_packet, start_len)));
    TEST_ASSERT_EQUAL(static_cast<int>(StreamSync::ReceiverGate::Action::AudioAccepted),
                      static_cast<int>(gate.accept(packet0, packet0_len)));
    TEST_ASSERT_EQUAL(static_cast<int>(StreamSync::ReceiverGate::Action::StopAccepted),
                      static_cast<int>(gate.accept(stop_packet, stop_len)));
    TEST_ASSERT_EQUAL(static_cast<int>(StreamSync::ReceiverGate::State::WaitingForStart),
                      static_cast<int>(gate.state()));
}                 

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_fileSegmenter_empty_input_is_explicitly_rejected);
    RUN_TEST(test_fileSegmenter_read_error_is_distinct_from_clean_eof);
    RUN_TEST(test_fileSegmenter_one_byte_round_trips);
    RUN_TEST(test_fileSegmenter_27_bytes_round_trip);
    RUN_TEST(test_fileSegmenter_exact_28_bytes_marks_final_segment_last);
    RUN_TEST(test_fileSegmenter_29_bytes_round_trip);
    RUN_TEST(test_fileSegmenter_55_bytes_round_trip);
    RUN_TEST(test_fileSegmenter_exact_56_bytes_marks_final_segment_last);
    RUN_TEST(test_fileSegmenter_57_bytes_round_trip);
    RUN_TEST(test_fileSegmenter_binary_fixtures_round_trip);
    RUN_TEST(test_fileSegmenter_maximum_size_completes_with_final_last);
    RUN_TEST(test_fileSegmenter_over_maximum_is_rejected_before_sequence_wrap);
    runReceiverSafetyTests();
    runProtocolV2Tests();
    runIntegrationReadinessTests();
    RUN_TEST(test_readReg_reads_value_and_formats_spi_command);
    RUN_TEST(test_readRfPowerLevel_decodes_rf_setup_bits);
    RUN_TEST(test_setRfPowerLevel_updates_packet_setup_and_register);
    RUN_TEST(test_audioPacket_encode_decode_round_trip);
    RUN_TEST(test_audioPacket_rejects_oversized_audio);
    RUN_TEST(test_audioReassembler_reassembles_packets_in_order);
    RUN_TEST(test_audioReassembler_accepts_small_forward_gap_and_records_missing);
    RUN_TEST(test_audioReassembler_rejects_large_forward_gap_and_resets);
    RUN_TEST(test_audioReassembler_ignores_duplicate_packet);
    RUN_TEST(test_writeReg_writes_register_and_formats_spi_command);
    RUN_TEST(test_powerUp_sets_power_bit_and_waits_for_startup);
    RUN_TEST(test_probe_restores_original_channel_after_check);
    RUN_TEST(test_initDefaults_programs_expected_registers);
    RUN_TEST(test_startRx_sets_rx_mode_and_raises_ce);
    RUN_TEST(test_transmitOnce_success_writes_payload_and_reports_success);
    RUN_TEST(test_transmitOnce_failure_clears_fifo_and_returns_false);
    RUN_TEST(test_transmitOnce_without_irq_wire_still_uses_status_polling);
    RUN_TEST(test_transmitOnce_rearm_preserves_runtime_power_level);
    RUN_TEST(test_readOnePacket_reads_payload_and_clears_rx_flag);
    RUN_TEST(test_readOnePacket_preserves_following_rx_payloads);
    RUN_TEST(test_radioManager_boot_success_transitions_to_standby);
    RUN_TEST(test_radioManager_boot_invalid_channel_sets_fault_code);
    RUN_TEST(test_radioManager_setPowerLevel_persists_across_reboot);
    RUN_TEST(test_radioManager_sendPayload_success_updates_status);
    RUN_TEST(test_radioManager_refreshSnapshot_reports_live_irq_state);
    RUN_TEST(test_radioManager_receivePayload_updates_rx_length);
    RUN_TEST(test_radioManager_hasPendingRx_reports_fifo_backlog_after_irq_clear);
    RUN_TEST(test_radioManager_hasPendingRx_true_when_rx_dr_set);
    RUN_TEST(test_radioManager_hasPendingRx_true_when_fifo_not_empty_without_rx_dr);
    RUN_TEST(test_radioManager_hasPendingRx_false_when_rx_dr_clear_and_fifo_empty);
    RUN_TEST(test_readOnePacket_does_not_flush_rx_after_successful_read);
    RUN_TEST(test_radioManager_sends_encoded_audio_as_fixed_width_zero_padded_payload);
    RUN_TEST(test_radioManager_startCw_updates_output_power);
    RUN_TEST(test_txHelpers_pacing_8_bytes_delays_1_ms_with_no_remainder);
    RUN_TEST(test_txHelpers_pacing_single_bytes_accumulate_to_1_ms);
    RUN_TEST(test_txHelpers_pacing_10_bytes_keeps_250_us_remainder);
    RUN_TEST(test_txHelpers_retry_succeeds_on_first_try);
    RUN_TEST(test_txHelpers_retry_succeeds_on_retry);
    RUN_TEST(test_txHelpers_retry_fails_after_all_attempts);
    RUN_TEST(test_rxDrain_processes_multiple_pending_packets);
    RUN_TEST(test_rxDrain_exits_on_receive_failure);
    RUN_TEST(test_frame_io_round_trip_preserves_record);
    RUN_TEST(test_validation_rejects_oversized_payload);
    RUN_TEST(test_morse_encode_e_creates_single_dot_event);
    RUN_TEST(test_morse_encode_word_gap_is_seven_dots);
    RUN_TEST(test_morse_render_formats_letters_and_words_on_one_line);
    RUN_TEST(test_stopContinuousCarrier_restores_demo_rf_setup);
    RUN_TEST(test_startContinuousCarrier_uses_cont_wave_when_supported);
    RUN_TEST(test_startContinuousCarrier_falls_back_to_payload_reuse_when_needed);
    RUN_TEST(test_audioPacket_decode_accepts_full_width_padded_packet);
    RUN_TEST(test_audioPacket_decode_accepts_exact_length_packet);
    RUN_TEST(test_audioPacket_decode_accepts_partially_padded_packet);
    RUN_TEST(test_audioPacket_decode_rejects_too_short_packet);
    RUN_TEST(test_audioPacket_decode_rejects_oversized_packet);
    RUN_TEST(test_audioPacket_decode_rejects_declared_audio_len_larger_than_max);
    RUN_TEST(test_readOnePacket_reads_fifo_backlog_even_if_rx_dr_is_clear);
    RUN_TEST(test_radioManager_hasPendingRx_reports_fifo_backlog_even_after_irq_clear);
    RUN_TEST(test_radioManager_receivePayload_reads_fifo_backlog_after_irq_clear);
    RUN_TEST(test_audioPacket_decode_accepts_every_valid_audio_len_as_padded_packet);
    RUN_TEST(test_audioPacket_decode_accepts_zero_audio_len);
    RUN_TEST(test_audioReassembler_accepts_padded_packets_in_order);
    RUN_TEST(test_readOnePacket_returns_false_when_no_pending_data);
    RUN_TEST(test_radioManager_receivePayload_without_data_sets_fault);
    RUN_TEST(test_transmitOnce_timeout_without_irq_returns_false_and_sets_timeout);
    RUN_TEST(test_radioManager_boot_probe_failure_sets_fault_code_1);
    RUN_TEST(test_streamSync_start_round_trip);
    RUN_TEST(test_streamSync_stop_round_trip);
    RUN_TEST(test_streamSync_remote_command_round_trip);
    RUN_TEST(test_streamSync_remote_command_rejects_oversized_text);
    RUN_TEST(test_streamSync_gate_ignores_nonzero_audio_before_sync);
    RUN_TEST(test_streamSync_gate_accepts_legacy_seq0_without_start);
    RUN_TEST(test_streamSync_gate_accepts_start_then_seq0);
    RUN_TEST(test_streamSync_gate_ignores_packet_one_until_seq0_arrives);
    RUN_TEST(test_streamSync_gate_ignores_duplicate_seq0_after_stream_starts);
    RUN_TEST(test_streamSync_gate_new_start_resets_current_stream);
    RUN_TEST(test_streamSync_gate_stop_returns_to_waiting_for_start);
    return UNITY_END();
}
