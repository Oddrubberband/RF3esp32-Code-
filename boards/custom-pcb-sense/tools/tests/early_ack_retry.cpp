#include "reliable_transfer_v2.hpp"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>

using ProtocolV2::ErrorCode;
using ProtocolV2::Frame;
using ProtocolV2::Packet;
using ProtocolV2::PacketType;
using ReliableTransferV2::ReadResult;
using ReliableTransferV2::ReadState;
using ReliableTransferV2::SenderEvent;
using ReliableTransferV2::SenderSession;
using ReliableTransferV2::SenderState;

namespace {
constexpr uint32_t kTransferId = 1;
constexpr uint64_t kFirstTestSend = 180;

struct TestFile {
    size_t offset = 0;
    static constexpr size_t size = 120;

    static bool reset(void* context)
    {
        static_cast<TestFile*>(context)->offset = 0;
        return true;
    }

    static ReadResult read(void* context, uint8_t* output, size_t capacity)
    {
        auto& file = *static_cast<TestFile*>(context);
        const auto amount = std::min(capacity, size - file.offset);
        std::memset(output, 0x5a, amount);
        file.offset += amount;
        return {amount, file.offset == size ? ReadState::EndOfFile : ReadState::MoreDataMayFollow};
    }
};

struct Attempt {
    TestFile file;
    SenderSession sender{&file, {TestFile::reset, TestFile::read}};

    Attempt()
    {
        ReliableTransferV2::Metadata meta;
        assert(ReliableTransferV2::inspectSource(&file, {TestFile::reset, TestFile::read}, meta) ==
               ReliableTransferV2::InspectResult::Ready);
        assert(sender.begin(kTransferId, meta, 0));
        assert(send(1) == PacketType::Start);
        assert(sender.tick(9) == SenderEvent::NoChange);
        Packet ready{};
        ready.type = PacketType::Ready;
        ready.transfer_id = kTransferId;
        ready.accepted = true;
        assert(receive(ready, 10) == SenderEvent::OutboundReady);
    }

    PacketType send(uint64_t when)
    {
        Frame frame{};
        Packet packet{};
        assert(sender.outboundFrame(frame));
        assert(ProtocolV2::decode(frame.data(), frame.size(), packet) == ProtocolV2::DecodeStatus::Ok);
        assert(sender.noteFrameSent(when));
        return packet.type;
    }

    SenderEvent receive(const Packet& packet, uint64_t when)
    {
        Frame frame{};
        assert(ProtocolV2::encode(packet, frame));
        return sender.onFrame(frame.data(), frame.size(), when);
    }

    SenderEvent ack(uint16_t sequence, uint64_t when)
    {
        Packet packet{};
        packet.type = PacketType::Ack;
        packet.transfer_id = kTransferId;
        packet.sequence = sequence;
        return receive(packet, when);
    }

    void qualify()
    {
        for (uint16_t sequence = 0; sequence < 3; ++sequence) {
            const uint64_t now = 30 + sequence * 40;
            assert(send(now) == PacketType::Data);
            assert(ack(sequence, now + 20) == SenderEvent::OutboundReady);
        }
        assert(sender.acknowledgedBytes() == 60);
    }
};

void lostAck()
{
    Attempt attempt;
    attempt.qualify();
    assert(attempt.send(kFirstTestSend) == PacketType::Data);
    uint64_t retry_delay = 0;
    while (attempt.sender.tick(kFirstTestSend + retry_delay) == SenderEvent::NoChange) ++retry_delay;
#ifdef EXPECT_EARLY_RETRY
    assert(retry_delay == 125 && attempt.sender.earlyDataRetryPending());
#else
    assert(retry_delay == 1000);
#endif
    assert(attempt.send(kFirstTestSend + retry_delay) == PacketType::Data);
    assert(attempt.ack(3, kFirstTestSend + retry_delay + 20) == SenderEvent::OutboundReady);
    assert(attempt.sender.acknowledgedBytes() == 80 && attempt.sender.totalRetries() == 1);
    assert(attempt.ack(3, kFirstTestSend + retry_delay + 21) == SenderEvent::Ignored);
    std::printf("Scripted missed ACK recovered in %llu ms\n", static_cast<unsigned long long>(retry_delay + 20));
}

#ifdef EXPECT_EARLY_RETRY
void delayedAck()
{
    Attempt attempt;
    attempt.qualify();
    assert(attempt.send(kFirstTestSend) == PacketType::Data);
    assert(attempt.sender.tick(kFirstTestSend + 125) == SenderEvent::OutboundReady);
    assert(attempt.send(kFirstTestSend + 126) == PacketType::Data);
    assert(attempt.sender.tick(kFirstTestSend + 379) == SenderEvent::NoChange);
    assert(attempt.ack(3, kFirstTestSend + 380) == SenderEvent::OutboundReady);
    assert(attempt.sender.acknowledgedBytes() == 80);
    assert(attempt.send(kFirstTestSend + 400) == PacketType::Data);
    assert(attempt.sender.tick(kFirstTestSend + 525) == SenderEvent::NoChange);
    assert(attempt.sender.tick(kFirstTestSend + 1400) == SenderEvent::OutboundReady);
    assert(!attempt.sender.earlyDataRetryPending());
}

void fullBudget()
{
    Attempt attempt;
    attempt.qualify();
    assert(attempt.send(kFirstTestSend) == PacketType::Data);
    assert(attempt.sender.tick(kFirstTestSend + 125) == SenderEvent::OutboundReady);
    assert(attempt.send(kFirstTestSend + 126) == PacketType::Data);
    for (uint32_t retry = 1; retry <= ProtocolV2::kMaximumRetries; ++retry) {
        const uint64_t deadline = kFirstTestSend + retry * ProtocolV2::kDataAckTimeoutMs;
        assert(attempt.sender.tick(deadline - 1) == SenderEvent::NoChange);
        assert(attempt.sender.tick(deadline) == SenderEvent::OutboundReady);
        assert(!attempt.sender.earlyDataRetryPending());
        assert(attempt.send(deadline) == PacketType::Data);
    }
    const uint64_t final = kFirstTestSend + (ProtocolV2::kMaximumRetries + 1) * ProtocolV2::kDataAckTimeoutMs;
    assert(attempt.sender.tick(final - 1) == SenderEvent::NoChange);
    assert(attempt.sender.tick(final) == SenderEvent::Failed);
    assert(attempt.sender.error() == ErrorCode::RetryExhausted);
    assert(attempt.sender.totalRetries() == ProtocolV2::kMaximumRetries + 1u);
}

void lateQueuedRetry()
{
    Attempt attempt;
    attempt.qualify();
    assert(attempt.send(kFirstTestSend) == PacketType::Data);
    assert(attempt.sender.tick(kFirstTestSend + 125) == SenderEvent::OutboundReady);
    assert(!attempt.sender.deferEarlyDataRetry(kFirstTestSend + 126, false));
    assert(attempt.sender.deferEarlyDataRetry(kFirstTestSend + 950, false));
    assert(!attempt.sender.earlyDataRetryPending() && attempt.sender.totalRetries() == 0);
    assert(attempt.sender.tick(kFirstTestSend + 951) == SenderEvent::NoChange);
    assert(attempt.ack(3, kFirstTestSend + 975) == SenderEvent::OutboundReady);
    assert(attempt.sender.acknowledgedBytes() == 80);
}

void queuedAckArrival()
{
    Attempt attempt;
    attempt.qualify();
    assert(attempt.send(kFirstTestSend) == PacketType::Data);
    assert(attempt.sender.tick(kFirstTestSend + 125) == SenderEvent::OutboundReady);
    assert(attempt.sender.deferEarlyDataRetry(kFirstTestSend + 130, true));
    assert(attempt.ack(3, kFirstTestSend + 131) == SenderEvent::OutboundReady);
    assert(attempt.sender.acknowledgedBytes() == 80 && attempt.sender.totalRetries() == 0);
}

void lateWakeAndCancel()
{
    Attempt attempt;
    attempt.qualify();
    assert(attempt.send(kFirstTestSend) == PacketType::Data);
    assert(attempt.sender.tick(kFirstTestSend + 950) == SenderEvent::NoChange);
    assert(!attempt.sender.earlyDataRetryPending());
    assert(attempt.sender.tick(kFirstTestSend + 1000) == SenderEvent::OutboundReady);
    assert(!attempt.sender.earlyDataRetryPending());
    assert(attempt.sender.cancel(kFirstTestSend + 1001) == SenderEvent::OutboundReady);
    assert(attempt.sender.state() == SenderState::Cancelling);
    assert(attempt.send(kFirstTestSend + 1002) == PacketType::Cancel);
    assert(attempt.sender.tick(kFirstTestSend + 1501) == SenderEvent::NoChange);
    assert(attempt.sender.tick(kFirstTestSend + 1502) == SenderEvent::OutboundReady);
}
#endif
}

int main()
{
    lostAck();
#ifdef EXPECT_EARLY_RETRY
    delayedAck();
    fullBudget();
    lateQueuedRetry();
    queuedAckArrival();
    lateWakeAndCancel();
    std::puts("PASS delayed and duplicate ACKs, full ordinary retry budget, late wake and cancellation");
#endif
}
