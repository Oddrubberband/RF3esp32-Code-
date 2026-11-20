#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "protocol_v2.hpp"

namespace ProtocolV2Test {

enum class Destination {
    Sender,
    Receiver,
};

enum class FaultKind {
    Drop,
    Duplicate,
    Corrupt,
    Delay,
};

struct FaultRule {
    Destination destination = Destination::Receiver;
    FaultKind kind = FaultKind::Drop;
    ProtocolV2::PacketType type = ProtocolV2::PacketType::Start;
    bool any_type = false;
    uint32_t sequence = 0;
    bool any_sequence = true;
    uint32_t occurrence = 1;
    uint32_t repeat_count = 1;
    uint32_t delay_ms = 0;
    size_t corrupt_offset = ProtocolV2::kFrameSize;
    uint8_t corrupt_mask = 0x01;
    uint32_t matches = 0;
};

class FakeDuplexTransport {
public:
    static constexpr size_t kMaxQueuedFrames = 256;
    static constexpr size_t kMaxRules = 64;

    explicit FakeDuplexTransport(uint32_t seed = 0x52463332u)
        : random_state_(seed == 0 ? 1u : seed)
    {
    }

    bool addRule(const FaultRule& rule)
    {
        if (rule_count_ >= rules_.size() || rule.occurrence == 0 ||
            rule.repeat_count == 0) {
            return false;
        }
        rules_[rule_count_++] = rule;
        return true;
    }

    bool send(Destination destination,
              const ProtocolV2::Frame& frame,
              uint64_t now_ms)
    {
        ProtocolV2::Packet packet{};
        const bool decoded = ProtocolV2::decode(
            frame.data(), frame.size(), packet) == ProtocolV2::DecodeStatus::Ok;

        for (size_t index = 0; index < rule_count_; ++index) {
            FaultRule& rule = rules_[index];
            if (!matches(rule, destination, decoded ? &packet : nullptr)) {
                continue;
            }
            ++rule.matches;
            if (rule.matches < rule.occurrence ||
                rule.matches >= rule.occurrence + rule.repeat_count) {
                continue;
            }

            switch (rule.kind) {
                case FaultKind::Drop:
                    return true;
                case FaultKind::Duplicate:
                    return enqueue(destination, frame, now_ms) &&
                           enqueue(destination, frame, now_ms);
                case FaultKind::Corrupt: {
                    ProtocolV2::Frame changed = frame;
                    const size_t offset = rule.corrupt_offset < changed.size()
                                              ? rule.corrupt_offset
                                              : static_cast<size_t>(nextRandom() % changed.size());
                    changed[offset] ^= rule.corrupt_mask == 0 ? 1u : rule.corrupt_mask;
                    return enqueue(destination, changed, now_ms);
                }
                case FaultKind::Delay:
                    return enqueue(destination, frame, now_ms + rule.delay_ms);
                default:
                    return false;
            }
        }

        return enqueue(destination, frame, now_ms);
    }

    bool pop(Destination destination,
             uint64_t now_ms,
             ProtocolV2::Frame& out)
    {
        for (size_t index = 0; index < queue_count_; ++index) {
            if (queue_[index].destination != destination ||
                queue_[index].deliver_at_ms > now_ms) {
                continue;
            }
            out = queue_[index].frame;
            for (size_t move = index + 1; move < queue_count_; ++move) {
                queue_[move - 1] = queue_[move];
            }
            --queue_count_;
            return true;
        }
        return false;
    }

    void resetSender()
    {
        clearQueuedFrames();
        ++sender_reset_count_;
    }

    void resetReceiver()
    {
        clearQueuedFrames();
        ++receiver_reset_count_;
    }

    void clearQueuedFrames() { queue_count_ = 0; }
    size_t queuedFrames() const { return queue_count_; }
    uint32_t senderResetCount() const { return sender_reset_count_; }
    uint32_t receiverResetCount() const { return receiver_reset_count_; }

private:
    struct QueuedFrame {
        Destination destination = Destination::Receiver;
        ProtocolV2::Frame frame{};
        uint64_t deliver_at_ms = 0;
    };

    bool enqueue(Destination destination,
                 const ProtocolV2::Frame& frame,
                 uint64_t deliver_at_ms)
    {
        if (queue_count_ >= queue_.size()) {
            return false;
        }
        queue_[queue_count_++] = QueuedFrame{destination, frame, deliver_at_ms};
        return true;
    }

    static uint32_t packetSequence(const ProtocolV2::Packet& packet)
    {
        if (packet.type == ProtocolV2::PacketType::Data ||
            packet.type == ProtocolV2::PacketType::Ack) {
            return packet.sequence;
        }
        if (packet.type == ProtocolV2::PacketType::Ready ||
            packet.type == ProtocolV2::PacketType::Nack) {
            return packet.expected_sequence;
        }
        return packet.relevant_sequence;
    }

    static bool matches(const FaultRule& rule,
                        Destination destination,
                        const ProtocolV2::Packet* packet)
    {
        if (rule.destination != destination) {
            return false;
        }
        if (!rule.any_type && (!packet || packet->type != rule.type)) {
            return false;
        }
        if (!rule.any_sequence && (!packet || packetSequence(*packet) != rule.sequence)) {
            return false;
        }
        return true;
    }

    uint32_t nextRandom()
    {
        uint32_t value = random_state_;
        value ^= value << 13;
        value ^= value >> 17;
        value ^= value << 5;
        random_state_ = value;
        return value;
    }

    std::array<QueuedFrame, kMaxQueuedFrames> queue_{};
    std::array<FaultRule, kMaxRules> rules_{};
    size_t queue_count_ = 0;
    size_t rule_count_ = 0;
    uint32_t random_state_ = 1;
    uint32_t sender_reset_count_ = 0;
    uint32_t receiver_reset_count_ = 0;
};

}  // namespace ProtocolV2Test
