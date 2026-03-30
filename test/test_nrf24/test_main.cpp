#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <vector>

#include <unity.h>

#include "../include/fake_hal.hpp"
#include "audio_packet.hpp"
#include "audio_reassembler.hpp"
#include "frame_io.hpp"
#include "morse.hpp"
#include "nrf24.hpp"
#include "radio_manager.hpp"
#include "validation.hpp"

void setUp(void)
{
}

void tearDown(void)
{
}

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

// Audio packet / reassembly tests
void test_audioPacket_encode_decode_round_trip(void)
{
    const uint8_t audio[] = {0x12, 0x34, 0x56};
    uint8_t packet[AudioPacket::kPacketBytes] = {};
    size_t packet_len = 0;

    const bool encoded = AudioPacket::encode(7, audio, sizeof(audio), true, false, packet, packet_len);
    TEST_ASSERT_TRUE(encoded);
    TEST_ASSERT_EQUAL_UINT32(AudioPacket::kHeaderBytes + sizeof(audio), static_cast<uint32_t>(packet_len));

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

void test_audioReassembler_rejects_out_of_order_packet(void)
{
    uint8_t packet0[AudioPacket::kPacketBytes] = {};
    uint8_t packet2[AudioPacket::kPacketBytes] = {};
    size_t len0 = 0;
    size_t len2 = 0;

    TEST_ASSERT_TRUE(AudioPacket::encode(0, reinterpret_cast<const uint8_t*>("abc"), 3, true, false, packet0, len0));
    TEST_ASSERT_TRUE(AudioPacket::encode(2, reinterpret_cast<const uint8_t*>("zz"), 2, false, true, packet2, len2));

    AudioReassembler reassembler;
    TEST_ASSERT_TRUE(reassembler.acceptPacket(packet0, len0));
    TEST_ASSERT_FALSE(reassembler.acceptPacket(packet2, len2));
    TEST_ASSERT_EQUAL(static_cast<int>(AudioReassemblyError::UnexpectedSequence),
                      static_cast<int>(reassembler.lastError()));
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
    TEST_ASSERT_EQUAL(1, hal.tx_trigger_count);
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
    TEST_ASSERT_TRUE(hal.rx_fifo.empty());
    TEST_ASSERT_EQUAL_UINT8(0, static_cast<uint8_t>(hal.regs[0x07] & (1 << 6)));
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
    TEST_ASSERT_EQUAL_UINT8(0x06, hal.regs[0x06]);
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
    TEST_ASSERT_EQUAL_UINT8(0x96, hal.regs[0x06]);
    TEST_ASSERT_TRUE(hal.last_payload_write.empty());
}

void test_startContinuousCarrier_falls_back_to_payload_reuse_when_needed(void)
{
    FakeHal hal;
    hal.supports_cont_wave = false;
    hal.regs[0x10] = 1;
    hal.regs[0x11] = 2;
    hal.regs[0x12] = 3;
    hal.regs[0x13] = 4;
    hal.regs[0x14] = 5;

    Nrf24 radio(hal);

    TEST_ASSERT_TRUE(radio.initDefaults(76));
    TEST_ASSERT_TRUE(radio.startContinuousCarrier(33, 0x04));

    TEST_ASSERT_TRUE(hal.ce_level);
    TEST_ASSERT_EQUAL_UINT8(33, hal.regs[0x05]);
    TEST_ASSERT_EQUAL_UINT8(0x14, hal.regs[0x06]);
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
    TEST_ASSERT_EQUAL_UINT8(0x06, hal.regs[0x06]);
    TEST_ASSERT_EQUAL_UINT8(1, hal.regs[0x10]);
    TEST_ASSERT_EQUAL_UINT8(2, hal.regs[0x11]);
    TEST_ASSERT_EQUAL_UINT8(3, hal.regs[0x12]);
    TEST_ASSERT_EQUAL_UINT8(4, hal.regs[0x13]);
    TEST_ASSERT_EQUAL_UINT8(5, hal.regs[0x14]);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_readReg_reads_value_and_formats_spi_command);
    RUN_TEST(test_readRfPowerLevel_decodes_rf_setup_bits);
    RUN_TEST(test_audioPacket_encode_decode_round_trip);
    RUN_TEST(test_audioPacket_rejects_oversized_audio);
    RUN_TEST(test_audioReassembler_reassembles_packets_in_order);
    RUN_TEST(test_audioReassembler_rejects_out_of_order_packet);
    RUN_TEST(test_writeReg_writes_register_and_formats_spi_command);
    RUN_TEST(test_powerUp_sets_power_bit_and_waits_for_startup);
    RUN_TEST(test_probe_restores_original_channel_after_check);
    RUN_TEST(test_initDefaults_programs_expected_registers);
    RUN_TEST(test_startRx_sets_rx_mode_and_raises_ce);
    RUN_TEST(test_transmitOnce_success_writes_payload_and_reports_success);
    RUN_TEST(test_transmitOnce_failure_clears_fifo_and_returns_false);
    RUN_TEST(test_transmitOnce_without_irq_wire_still_uses_status_polling);
    RUN_TEST(test_readOnePacket_reads_payload_and_clears_rx_flag);
    RUN_TEST(test_radioManager_boot_success_transitions_to_standby);
    RUN_TEST(test_radioManager_boot_invalid_channel_sets_fault_code);
    RUN_TEST(test_radioManager_sendPayload_success_updates_status);
    RUN_TEST(test_radioManager_refreshSnapshot_reports_live_irq_state);
    RUN_TEST(test_radioManager_receivePayload_updates_rx_length);
    RUN_TEST(test_radioManager_startCw_updates_output_power);
    RUN_TEST(test_frame_io_round_trip_preserves_record);
    RUN_TEST(test_validation_rejects_oversized_payload);
    RUN_TEST(test_morse_encode_e_creates_single_dot_event);
    RUN_TEST(test_morse_encode_word_gap_is_seven_dots);
    RUN_TEST(test_morse_render_formats_letters_and_words_on_one_line);
    RUN_TEST(test_stopContinuousCarrier_restores_demo_rf_setup);
    RUN_TEST(test_startContinuousCarrier_uses_cont_wave_when_supported);
    RUN_TEST(test_startContinuousCarrier_falls_back_to_payload_reuse_when_needed);
    return UNITY_END();
}
