#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "protocol_v2.hpp"

// ReliableTransferV2 contains the hardware-independent streaming sender and
// receiver state machines. Callers supply explicit source/sink callbacks and
// monotonic timestamps; neither state machine owns a radio, filesystem, task,
// or wall clock.
namespace ReliableTransferV2 {

inline bool isInternalTransferName(std::string_view name)
{
    constexpr std::string_view kPartialSuffix = ".part";
    return name.size() >= kPartialSuffix.size() &&
           name.substr(name.size() - kPartialSuffix.size()) == kPartialSuffix;
}

enum class ReadState {
    MoreDataMayFollow,
    EndOfFile,
    Error,
};

struct ReadResult {
    size_t bytes_read = 0;
    ReadState state = ReadState::MoreDataMayFollow;
};

struct SourceCallbacks {
    bool (*reset)(void* context) = nullptr;
    ReadResult (*read)(void* context, uint8_t* out, size_t capacity) = nullptr;
};

enum class InspectResult {
    Ready,
    InvalidCallbacks,
    ResetFailed,
    ReadFailed,
    UnsupportedSize,
};

struct Metadata {
    uint32_t total_size = 0;
    uint32_t total_packets = 0;
    uint32_t crc32 = 0;
};

inline InspectResult inspectSource(void* context,
                                   SourceCallbacks source,
                                   Metadata& out)
{
    out = Metadata{};
    if (!source.reset || !source.read) {
        return InspectResult::InvalidCallbacks;
    }
    if (!source.reset(context)) {
        return InspectResult::ResetFailed;
    }

    std::array<uint8_t, 128> buffer{};
    ProtocolV2::Crc32 crc;
    uint64_t total = 0;
    bool finished = false;
    while (!finished) {
        const ReadResult result = source.read(context, buffer.data(), buffer.size());
        if (result.bytes_read > buffer.size() || result.state == ReadState::Error) {
            (void)source.reset(context);
            return InspectResult::ReadFailed;
        }
        if (result.bytes_read == 0) {
            if (result.state != ReadState::EndOfFile) {
                (void)source.reset(context);
                return InspectResult::ReadFailed;
            }
            finished = true;
            continue;
        }

        total += result.bytes_read;
        if (total > ProtocolV2::kMaxFileSize) {
            (void)source.reset(context);
            return InspectResult::UnsupportedSize;
        }
        crc.update(buffer.data(), result.bytes_read);
        finished = result.state == ReadState::EndOfFile;
    }

    if (!source.reset(context)) {
        return InspectResult::ResetFailed;
    }

    out.total_size = static_cast<uint32_t>(total);
    out.total_packets = ProtocolV2::packetCountForSize(out.total_size);
    out.crc32 = crc.value();
    return InspectResult::Ready;
}

enum class SenderState {
    Idle,
    Preparing,
    WaitingForReady,
    SendingData,
    WaitingForDataAck,
    WaitingForComplete,
    Completed,
    Cancelling,
    Cancelled,
    Failed,
};

enum class SenderEvent {
    NoChange,
    OutboundReady,
    Ignored,
    Completed,
    Cancelled,
    Failed,
};

class SenderSession {
public:
    SenderSession(void* source_context,
                  SourceCallbacks source,
                  uint32_t control_timeout_ms = ProtocolV2::kControlResponseTimeoutMs,
                  uint32_t data_timeout_ms = ProtocolV2::kDataAckTimeoutMs,
                  uint8_t maximum_retries = ProtocolV2::kMaximumRetries,
                  uint32_t inactivity_timeout_ms = ProtocolV2::kSenderInactivityTimeoutMs)
        : source_context_(source_context),
          source_(source),
          control_timeout_ms_(control_timeout_ms),
          data_timeout_ms_(data_timeout_ms),
          maximum_retries_(maximum_retries),
          inactivity_timeout_ms_(inactivity_timeout_ms)
    {
    }

    bool begin(uint32_t transfer_id, const Metadata& metadata, uint64_t now_ms)
    {
        reset();
        state_ = SenderState::Preparing;
        if (transfer_id == 0 || !source_.reset || !source_.read) {
            return fail(ProtocolV2::ErrorCode::StateViolation);
        }
        if (!ProtocolV2::metadataSupported(metadata.total_size,
                                           metadata.total_packets)) {
            return fail(ProtocolV2::ErrorCode::UnsupportedSize);
        }
        if (!source_.reset(source_context_)) {
            return fail(ProtocolV2::ErrorCode::SourceRead);
        }

        transfer_id_ = transfer_id;
        metadata_ = metadata;
        last_activity_ms_ = now_ms;
        ProtocolV2::Packet packet{};
        packet.type = ProtocolV2::PacketType::Start;
        packet.transfer_id = transfer_id_;
        packet.total_size = metadata_.total_size;
        packet.total_packets = metadata_.total_packets;
        packet.crc32 = metadata_.crc32;
        if (!setOutbound(packet)) {
            return fail(ProtocolV2::ErrorCode::StateViolation);
        }
        state_ = SenderState::WaitingForReady;
        return true;
    }

    void reset()
    {
        state_ = SenderState::Idle;
        error_ = ProtocolV2::ErrorCode::None;
        metadata_ = Metadata{};
        transfer_id_ = 0;
        current_sequence_ = 0;
        current_payload_length_ = 0;
        acknowledged_packets_ = 0;
        acknowledged_bytes_ = 0;
        retry_count_ = 0;
        total_retries_ = 0;
        last_activity_ms_ = 0;
        response_deadline_ms_ = 0;
        outbound_ready_ = false;
        waiting_for_response_ = false;
        peer_complete_ = false;
        outbound_.fill(0);
    }

    bool outboundFrame(ProtocolV2::Frame& out) const
    {
        if (!outbound_ready_) {
            return false;
        }
        out = outbound_;
        return true;
    }

    bool noteFrameSent(uint64_t now_ms)
    {
        if (!outbound_ready_ || terminal()) {
            return false;
        }
        outbound_ready_ = false;
        waiting_for_response_ = true;
        response_deadline_ms_ = now_ms + responseTimeoutMs();
        return true;
    }

    SenderEvent onTransportFailure(uint64_t now_ms)
    {
        if (terminal()) {
            return terminalEvent();
        }
        waiting_for_response_ = false;
        response_deadline_ms_ = now_ms;
        return scheduleRetry();
    }

    SenderEvent onFrame(const uint8_t* frame, size_t length, uint64_t now_ms)
    {
        if (terminal()) {
            return terminalEvent();
        }

        ProtocolV2::Packet packet{};
        if (ProtocolV2::decode(frame, length, packet) != ProtocolV2::DecodeStatus::Ok) {
            return SenderEvent::Ignored;
        }
        if (packet.transfer_id != transfer_id_) {
            return SenderEvent::Ignored;
        }

        if (packet.type == ProtocolV2::PacketType::Error) {
            last_activity_ms_ = now_ms;
            waiting_for_response_ = false;
            if (state_ == SenderState::Cancelling &&
                packet.error == ProtocolV2::ErrorCode::Cancelled) {
                state_ = SenderState::Cancelled;
                error_ = ProtocolV2::ErrorCode::Cancelled;
                return SenderEvent::Cancelled;
            }
            fail(packet.error);
            return SenderEvent::Failed;
        }

        switch (state_) {
            case SenderState::WaitingForReady:
                if (packet.type != ProtocolV2::PacketType::Ready) {
                    return SenderEvent::Ignored;
                }
                last_activity_ms_ = now_ms;
                waiting_for_response_ = false;
                if (!packet.accepted) {
                    fail(packet.error);
                    return SenderEvent::Failed;
                }
                if (packet.expected_sequence != 0) {
                    fail(ProtocolV2::ErrorCode::UnexpectedSequence);
                    return SenderEvent::Failed;
                }
                retry_count_ = 0;
                return prepareNextDataOrEnd() ? SenderEvent::OutboundReady
                                              : SenderEvent::Failed;

            case SenderState::WaitingForDataAck:
                if (packet.type == ProtocolV2::PacketType::Nack) {
                    last_activity_ms_ = now_ms;
                    waiting_for_response_ = false;
                    if (packet.expected_sequence != current_sequence_) {
                        fail(ProtocolV2::ErrorCode::UnexpectedSequence);
                        return SenderEvent::Failed;
                    }
                    return scheduleRetry();
                }
                if (packet.type != ProtocolV2::PacketType::Ack) {
                    return SenderEvent::Ignored;
                }
                if (packet.sequence < current_sequence_) {
                    return SenderEvent::Ignored;
                }
                if (packet.sequence != current_sequence_) {
                    fail(ProtocolV2::ErrorCode::UnexpectedSequence);
                    return SenderEvent::Failed;
                }
                last_activity_ms_ = now_ms;
                waiting_for_response_ = false;
                retry_count_ = 0;
                ++acknowledged_packets_;
                acknowledged_bytes_ += current_payload_length_;
                ++current_sequence_;
                return prepareNextDataOrEnd() ? SenderEvent::OutboundReady
                                              : SenderEvent::Failed;

            case SenderState::WaitingForComplete:
                if (packet.type != ProtocolV2::PacketType::Complete) {
                    return SenderEvent::Ignored;
                }
                last_activity_ms_ = now_ms;
                if (packet.total_size != metadata_.total_size ||
                    packet.crc32 != metadata_.crc32 ||
                    packet.error != ProtocolV2::ErrorCode::None) {
                    fail(ProtocolV2::ErrorCode::InvalidMetadata);
                    return SenderEvent::Failed;
                }
                waiting_for_response_ = false;
                peer_complete_ = true;
                state_ = SenderState::Completed;
                error_ = ProtocolV2::ErrorCode::None;
                return SenderEvent::Completed;

            case SenderState::Cancelling:
                return SenderEvent::Ignored;

            default:
                fail(ProtocolV2::ErrorCode::StateViolation);
                return SenderEvent::Failed;
        }
    }

    SenderEvent tick(uint64_t now_ms)
    {
        if (terminal()) {
            return terminalEvent();
        }
        if (now_ms < last_activity_ms_ ||
            now_ms - last_activity_ms_ >= inactivity_timeout_ms_) {
            if (state_ == SenderState::Cancelling) {
                state_ = SenderState::Cancelled;
                error_ = ProtocolV2::ErrorCode::Cancelled;
                return SenderEvent::Cancelled;
            }
            fail(ProtocolV2::ErrorCode::Timeout);
            return SenderEvent::Failed;
        }
        if (!waiting_for_response_ || now_ms < response_deadline_ms_) {
            return SenderEvent::NoChange;
        }
        waiting_for_response_ = false;
        return scheduleRetry();
    }

    SenderEvent cancel(uint64_t now_ms,
                       ProtocolV2::ErrorCode reason = ProtocolV2::ErrorCode::Cancelled)
    {
        if (state_ == SenderState::Cancelled) {
            return SenderEvent::Cancelled;
        }
        if (terminal()) {
            return terminalEvent();
        }
        ProtocolV2::Packet packet{};
        packet.type = ProtocolV2::PacketType::Cancel;
        packet.transfer_id = transfer_id_;
        packet.error = reason == ProtocolV2::ErrorCode::None
                           ? ProtocolV2::ErrorCode::Cancelled : reason;
        if (!setOutbound(packet)) {
            fail(ProtocolV2::ErrorCode::StateViolation);
            return SenderEvent::Failed;
        }
        state_ = SenderState::Cancelling;
        error_ = ProtocolV2::ErrorCode::Cancelled;
        retry_count_ = 0;
        waiting_for_response_ = false;
        last_activity_ms_ = now_ms;
        return SenderEvent::OutboundReady;
    }

    SenderState state() const { return state_; }
    ProtocolV2::ErrorCode error() const { return error_; }
    uint32_t transferId() const { return transfer_id_; }
    uint32_t currentSequence() const { return current_sequence_; }
    uint32_t acknowledgedPackets() const { return acknowledged_packets_; }
    uint32_t acknowledgedBytes() const { return acknowledged_bytes_; }
    uint8_t retryCount() const { return retry_count_; }
    uint32_t totalRetries() const { return total_retries_; }
    uint32_t totalSize() const { return metadata_.total_size; }
    uint32_t totalPackets() const { return metadata_.total_packets; }
    uint32_t crc32() const { return metadata_.crc32; }
    bool peerComplete() const { return peer_complete_; }
    bool terminal() const
    {
        return state_ == SenderState::Completed ||
               state_ == SenderState::Cancelled ||
               state_ == SenderState::Failed;
    }

private:
    bool setOutbound(const ProtocolV2::Packet& packet)
    {
        if (!ProtocolV2::encode(packet, outbound_)) {
            return false;
        }
        outbound_ready_ = true;
        waiting_for_response_ = false;
        response_deadline_ms_ = 0;
        return true;
    }

    bool prepareNextDataOrEnd()
    {
        if (current_sequence_ >= metadata_.total_packets) {
            ProtocolV2::Packet end{};
            end.type = ProtocolV2::PacketType::End;
            end.transfer_id = transfer_id_;
            end.total_size = metadata_.total_size;
            end.total_packets = metadata_.total_packets;
            end.crc32 = metadata_.crc32;
            if (!setOutbound(end)) {
                return fail(ProtocolV2::ErrorCode::StateViolation);
            }
            state_ = SenderState::WaitingForComplete;
            return true;
        }

        const size_t expected = ProtocolV2::expectedPayloadLength(
            metadata_.total_size, metadata_.total_packets, current_sequence_);
        if (expected == 0 || current_sequence_ > UINT16_MAX) {
            return fail(ProtocolV2::ErrorCode::UnsupportedSize);
        }

        ProtocolV2::Packet data{};
        data.type = ProtocolV2::PacketType::Data;
        data.transfer_id = transfer_id_;
        data.sequence = static_cast<uint16_t>(current_sequence_);
        data.payload_length = static_cast<uint8_t>(expected);

        size_t offset = 0;
        while (offset < expected) {
            const ReadResult result = source_.read(
                source_context_, data.payload.data() + offset, expected - offset);
            if (result.bytes_read > expected - offset ||
                result.state == ReadState::Error || result.bytes_read == 0) {
                return fail(ProtocolV2::ErrorCode::SourceRead);
            }
            offset += result.bytes_read;
            if (result.state == ReadState::EndOfFile && offset < expected) {
                return fail(ProtocolV2::ErrorCode::SourceRead);
            }
        }

        current_payload_length_ = static_cast<uint8_t>(expected);
        if (!setOutbound(data)) {
            return fail(ProtocolV2::ErrorCode::StateViolation);
        }
        state_ = SenderState::WaitingForDataAck;
        return true;
    }

    SenderEvent scheduleRetry()
    {
        if (retry_count_ >= maximum_retries_) {
            if (state_ == SenderState::Cancelling) {
                state_ = SenderState::Cancelled;
                error_ = ProtocolV2::ErrorCode::Cancelled;
                outbound_ready_ = false;
                return SenderEvent::Cancelled;
            }
            fail(ProtocolV2::ErrorCode::RetryExhausted);
            return SenderEvent::Failed;
        }
        ++retry_count_;
        ++total_retries_;
        outbound_ready_ = true;
        waiting_for_response_ = false;
        return SenderEvent::OutboundReady;
    }

    uint32_t responseTimeoutMs() const
    {
        return state_ == SenderState::WaitingForDataAck
                   ? data_timeout_ms_ : control_timeout_ms_;
    }

    bool fail(ProtocolV2::ErrorCode error)
    {
        state_ = SenderState::Failed;
        error_ = error == ProtocolV2::ErrorCode::None
                     ? ProtocolV2::ErrorCode::StateViolation : error;
        outbound_ready_ = false;
        waiting_for_response_ = false;
        return false;
    }

    SenderEvent terminalEvent() const
    {
        if (state_ == SenderState::Completed) {
            return SenderEvent::Completed;
        }
        if (state_ == SenderState::Cancelled) {
            return SenderEvent::Cancelled;
        }
        return SenderEvent::Failed;
    }

    void* source_context_ = nullptr;
    SourceCallbacks source_{};
    uint32_t control_timeout_ms_ = ProtocolV2::kControlResponseTimeoutMs;
    uint32_t data_timeout_ms_ = ProtocolV2::kDataAckTimeoutMs;
    uint8_t maximum_retries_ = ProtocolV2::kMaximumRetries;
    uint32_t inactivity_timeout_ms_ = ProtocolV2::kSenderInactivityTimeoutMs;
    SenderState state_ = SenderState::Idle;
    ProtocolV2::ErrorCode error_ = ProtocolV2::ErrorCode::None;
    Metadata metadata_{};
    uint32_t transfer_id_ = 0;
    uint32_t current_sequence_ = 0;
    uint8_t current_payload_length_ = 0;
    uint32_t acknowledged_packets_ = 0;
    uint32_t acknowledged_bytes_ = 0;
    uint8_t retry_count_ = 0;
    uint32_t total_retries_ = 0;
    uint64_t last_activity_ms_ = 0;
    uint64_t response_deadline_ms_ = 0;
    ProtocolV2::Frame outbound_{};
    bool outbound_ready_ = false;
    bool waiting_for_response_ = false;
    bool peer_complete_ = false;
};

enum class SinkPrepareResult {
    Ready,
    CleanupFailed,
    OpenFailed,
};

struct SinkCallbacks {
    SinkPrepareResult (*prepare)(void* context,
                                 uint32_t transfer_id,
                                 uint32_t total_size) = nullptr;
    size_t (*write)(void* context, const uint8_t* data, size_t length) = nullptr;
    bool (*close)(void* context) = nullptr;
    bool (*publish)(void* context) = nullptr;
    bool (*remove_partial)(void* context) = nullptr;
};

enum class ReceiverState {
    Idle,
    Receiving,
    WaitingForEnd,
    Verifying,
    Publishing,
    Completed,
    Cancelled,
    Failed,
};

enum class ReceiverEvent {
    Ignored,
    ResponseReady,
    DataAccepted,
    Duplicate,
    Completed,
    Cancelled,
    TimedOut,
    Failed,
};

class ReceiverSession {
public:
    ReceiverSession(void* sink_context,
                    SinkCallbacks sink,
                    uint32_t inactivity_timeout_ms = ProtocolV2::kReceiverInactivityTimeoutMs)
        : sink_context_(sink_context),
          sink_(sink),
          inactivity_timeout_ms_(inactivity_timeout_ms)
    {
    }

    ReceiverEvent onFrame(const uint8_t* frame,
                          size_t length,
                          uint64_t now_ms,
                          ProtocolV2::Frame& response)
    {
        ProtocolV2::Packet packet{};
        const ProtocolV2::DecodeStatus decoded = ProtocolV2::decode(frame, length, packet);
        if (decoded != ProtocolV2::DecodeStatus::Ok) {
            uint32_t transfer_id = 0;
            if (!ProtocolV2::peekTransferId(frame, length, transfer_id)) {
                return ReceiverEvent::Ignored;
            }
            const ProtocolV2::ErrorCode error =
                decoded == ProtocolV2::DecodeStatus::UnsupportedVersion
                    ? ProtocolV2::ErrorCode::UnsupportedVersion
                    : ProtocolV2::ErrorCode::InvalidFrame;
            if (active() && transfer_id == transfer_id_) {
                return failWithResponse(error, expected_sequence_, response);
            }
            return makeError(transfer_id, error, 0, response)
                       ? ReceiverEvent::ResponseReady : ReceiverEvent::Failed;
        }
        return onPacket(packet, now_ms, response);
    }

    ReceiverEvent onPacket(const ProtocolV2::Packet& packet,
                           uint64_t now_ms,
                           ProtocolV2::Frame& response)
    {
        if (packet.type == ProtocolV2::PacketType::Start) {
            return handleStart(packet, now_ms, response);
        }

        if (packet.transfer_id != transfer_id_) {
            return ReceiverEvent::Ignored;
        }

        if (packet.type == ProtocolV2::PacketType::Cancel) {
            if (state_ == ReceiverState::Cancelled) {
                return makeError(packet.transfer_id,
                                 ProtocolV2::ErrorCode::Cancelled,
                                 expected_sequence_, response)
                           ? ReceiverEvent::Cancelled : ReceiverEvent::Failed;
            }
            if (!active()) {
                return ReceiverEvent::Ignored;
            }
            last_activity_ms_ = now_ms;
            (void)abortStorage(ProtocolV2::ErrorCode::Cancelled,
                               ReceiverState::Cancelled);
            return makeError(packet.transfer_id,
                             ProtocolV2::ErrorCode::Cancelled,
                             expected_sequence_, response)
                       ? ReceiverEvent::Cancelled : ReceiverEvent::Failed;
        }

        if (packet.type == ProtocolV2::PacketType::Data) {
            return handleData(packet, now_ms, response);
        }
        if (packet.type == ProtocolV2::PacketType::End) {
            return handleEnd(packet, now_ms, response);
        }

        if (active()) {
            last_activity_ms_ = now_ms;
            return makeError(packet.transfer_id,
                             ProtocolV2::ErrorCode::UnexpectedPacket,
                             expected_sequence_, response)
                       ? ReceiverEvent::ResponseReady : ReceiverEvent::Failed;
        }
        return ReceiverEvent::Ignored;
    }

    ReceiverEvent tick(uint64_t now_ms, ProtocolV2::Frame& response)
    {
        if (!active()) {
            return ReceiverEvent::Ignored;
        }
        if (now_ms >= last_activity_ms_ &&
            now_ms - last_activity_ms_ < inactivity_timeout_ms_) {
            return ReceiverEvent::Ignored;
        }
        const uint32_t id = transfer_id_;
        (void)abortStorage(ProtocolV2::ErrorCode::Timeout, ReceiverState::Failed);
        if (!makeError(id, ProtocolV2::ErrorCode::Timeout,
                       expected_sequence_, response)) {
            return ReceiverEvent::Failed;
        }
        return ReceiverEvent::TimedOut;
    }

    bool reset()
    {
        const bool cleaned = cleanupStorage();
        if (!cleaned) {
            state_ = ReceiverState::Failed;
            error_ = ProtocolV2::ErrorCode::CleanupFailed;
            return false;
        }
        clearSession();
        return true;
    }

    bool abort(ProtocolV2::ErrorCode error = ProtocolV2::ErrorCode::Cancelled)
    {
        if (!active() && !storage_open_ && !storage_present_) {
            return true;
        }
        return abortStorage(
            error == ProtocolV2::ErrorCode::None
                ? ProtocolV2::ErrorCode::Cancelled : error,
            ReceiverState::Cancelled);
    }

    ReceiverState state() const { return state_; }
    ProtocolV2::ErrorCode error() const { return error_; }
    bool active() const
    {
        return state_ == ReceiverState::Receiving ||
               state_ == ReceiverState::WaitingForEnd ||
               state_ == ReceiverState::Verifying ||
               state_ == ReceiverState::Publishing;
    }
    bool complete() const { return state_ == ReceiverState::Completed; }
    bool cleanupFailed() const { return cleanup_failed_; }
    uint32_t transferId() const { return transfer_id_; }
    uint32_t expectedSequence() const { return expected_sequence_; }
    uint32_t acceptedPackets() const { return accepted_packets_; }
    uint32_t acceptedBytes() const { return accepted_bytes_; }
    uint32_t totalPackets() const { return metadata_.total_packets; }
    uint32_t totalSize() const { return metadata_.total_size; }
    uint32_t expectedCrc32() const { return metadata_.crc32; }
    uint32_t calculatedCrc32() const { return crc_.value(); }
    uint64_t lastActivityMs() const { return last_activity_ms_; }
    uint32_t timeoutMs() const { return inactivity_timeout_ms_; }

private:
    bool callbacksValid() const
    {
        return sink_.prepare && sink_.write && sink_.close &&
               sink_.publish && sink_.remove_partial;
    }

    ReceiverEvent handleStart(const ProtocolV2::Packet& packet,
                              uint64_t now_ms,
                              ProtocolV2::Frame& response)
    {
        const Metadata incoming{
            packet.total_size,
            packet.total_packets,
            packet.crc32,
        };
        if (!ProtocolV2::metadataSupported(incoming.total_size,
                                           incoming.total_packets)) {
            return makeReady(packet.transfer_id, false, 0,
                             ProtocolV2::ErrorCode::UnsupportedSize, response)
                       ? ReceiverEvent::ResponseReady : ReceiverEvent::Failed;
        }

        if (active()) {
            if (packet.transfer_id == transfer_id_ && metadataEqual(incoming, metadata_)) {
                last_activity_ms_ = now_ms;
                return makeReady(packet.transfer_id, true, expected_sequence_,
                                 ProtocolV2::ErrorCode::None, response)
                           ? ReceiverEvent::ResponseReady : ReceiverEvent::Failed;
            }
            return makeReady(packet.transfer_id, false, 0,
                             ProtocolV2::ErrorCode::Busy, response)
                       ? ReceiverEvent::ResponseReady : ReceiverEvent::Failed;
        }

        if ((storage_open_ || storage_present_) && !cleanupStorage()) {
            state_ = ReceiverState::Failed;
            error_ = ProtocolV2::ErrorCode::CleanupFailed;
            return makeReady(packet.transfer_id, false, 0,
                             ProtocolV2::ErrorCode::CleanupFailed, response)
                       ? ReceiverEvent::Failed : ReceiverEvent::Failed;
        }
        if (!callbacksValid()) {
            state_ = ReceiverState::Failed;
            error_ = ProtocolV2::ErrorCode::StateViolation;
            return makeReady(packet.transfer_id, false, 0,
                             error_, response)
                       ? ReceiverEvent::Failed : ReceiverEvent::Failed;
        }

        clearSession();
        transfer_id_ = packet.transfer_id;
        metadata_ = incoming;
        const SinkPrepareResult prepared =
            sink_.prepare(sink_context_, transfer_id_, metadata_.total_size);
        if (prepared != SinkPrepareResult::Ready) {
            error_ = prepared == SinkPrepareResult::CleanupFailed
                         ? ProtocolV2::ErrorCode::CleanupFailed
                         : ProtocolV2::ErrorCode::SinkOpen;
            state_ = ReceiverState::Failed;
            storage_present_ = prepared == SinkPrepareResult::CleanupFailed;
            return makeReady(packet.transfer_id, false, 0, error_, response)
                       ? ReceiverEvent::Failed : ReceiverEvent::Failed;
        }

        storage_open_ = true;
        storage_present_ = true;
        crc_.reset();
        last_activity_ms_ = now_ms;
        state_ = metadata_.total_packets == 0
                     ? ReceiverState::WaitingForEnd : ReceiverState::Receiving;
        error_ = ProtocolV2::ErrorCode::None;
        return makeReady(packet.transfer_id, true, 0,
                         ProtocolV2::ErrorCode::None, response)
                   ? ReceiverEvent::ResponseReady : ReceiverEvent::Failed;
    }

    ReceiverEvent handleData(const ProtocolV2::Packet& packet,
                             uint64_t now_ms,
                             ProtocolV2::Frame& response)
    {
        if (!active()) {
            return ReceiverEvent::Ignored;
        }
        last_activity_ms_ = now_ms;

        const uint32_t sequence = packet.sequence;
        if (sequence < expected_sequence_) {
            return makeAck(packet.sequence, response)
                       ? ReceiverEvent::Duplicate : ReceiverEvent::Failed;
        }
        if (sequence > expected_sequence_) {
            return makeNack(expected_sequence_,
                            ProtocolV2::ErrorCode::UnexpectedSequence, response)
                       ? ReceiverEvent::ResponseReady : ReceiverEvent::Failed;
        }
        if (state_ == ReceiverState::WaitingForEnd ||
            expected_sequence_ >= metadata_.total_packets) {
            return failWithResponse(ProtocolV2::ErrorCode::PacketCountMismatch,
                                    expected_sequence_, response);
        }

        const size_t expected_length = ProtocolV2::expectedPayloadLength(
            metadata_.total_size, metadata_.total_packets, expected_sequence_);
        if (expected_length == 0 || packet.payload_length != expected_length) {
            return failWithResponse(ProtocolV2::ErrorCode::ByteCountMismatch,
                                    expected_sequence_, response);
        }

        const size_t written = sink_.write(
            sink_context_, packet.payload.data(), packet.payload_length);
        if (written != packet.payload_length) {
            return failWithResponse(ProtocolV2::ErrorCode::SinkWrite,
                                    expected_sequence_, response);
        }

        crc_.update(packet.payload.data(), packet.payload_length);
        ++accepted_packets_;
        accepted_bytes_ += packet.payload_length;
        ++expected_sequence_;
        if (expected_sequence_ == metadata_.total_packets) {
            state_ = ReceiverState::WaitingForEnd;
        }
        return makeAck(packet.sequence, response)
                   ? ReceiverEvent::DataAccepted : ReceiverEvent::Failed;
    }

    ReceiverEvent handleEnd(const ProtocolV2::Packet& packet,
                            uint64_t now_ms,
                            ProtocolV2::Frame& response)
    {
        if (state_ == ReceiverState::Completed) {
            if (packet.total_size == metadata_.total_size &&
                packet.total_packets == metadata_.total_packets &&
                packet.crc32 == metadata_.crc32) {
                return makeComplete(response)
                           ? ReceiverEvent::Completed : ReceiverEvent::Failed;
            }
            return ReceiverEvent::Ignored;
        }
        if (!active()) {
            return ReceiverEvent::Ignored;
        }
        last_activity_ms_ = now_ms;
        if (state_ != ReceiverState::WaitingForEnd) {
            return makeNack(expected_sequence_,
                            ProtocolV2::ErrorCode::UnexpectedSequence, response)
                       ? ReceiverEvent::ResponseReady : ReceiverEvent::Failed;
        }
        if (packet.total_packets != metadata_.total_packets ||
            packet.total_packets != accepted_packets_) {
            return failWithResponse(ProtocolV2::ErrorCode::PacketCountMismatch,
                                    expected_sequence_, response);
        }
        if (packet.total_size != metadata_.total_size ||
            packet.total_size != accepted_bytes_) {
            return failWithResponse(ProtocolV2::ErrorCode::ByteCountMismatch,
                                    expected_sequence_, response);
        }
        if (packet.crc32 != metadata_.crc32) {
            return failWithResponse(ProtocolV2::ErrorCode::InvalidMetadata,
                                    expected_sequence_, response);
        }

        state_ = ReceiverState::Verifying;
        storage_open_ = false;
        if (!sink_.close(sink_context_)) {
            return failWithResponse(ProtocolV2::ErrorCode::SinkClose,
                                    expected_sequence_, response);
        }
        if (crc_.value() != metadata_.crc32) {
            return failWithResponse(ProtocolV2::ErrorCode::CrcMismatch,
                                    expected_sequence_, response);
        }

        state_ = ReceiverState::Publishing;
        if (!sink_.publish(sink_context_)) {
            return failWithResponse(ProtocolV2::ErrorCode::PublicationFailed,
                                    expected_sequence_, response);
        }
        storage_present_ = false;
        state_ = ReceiverState::Completed;
        error_ = ProtocolV2::ErrorCode::None;
        return makeComplete(response)
                   ? ReceiverEvent::Completed : ReceiverEvent::Failed;
    }

    ReceiverEvent failWithResponse(ProtocolV2::ErrorCode error,
                                   uint32_t relevant_sequence,
                                   ProtocolV2::Frame& response)
    {
        const uint32_t id = transfer_id_;
        (void)abortStorage(error, ReceiverState::Failed);
        if (!makeError(id, error, relevant_sequence, response)) {
            return ReceiverEvent::Failed;
        }
        return ReceiverEvent::Failed;
    }

    bool abortStorage(ProtocolV2::ErrorCode error, ReceiverState final_state)
    {
        state_ = final_state;
        error_ = error;
        const bool cleaned = cleanupStorage();
        if (!cleaned) {
            error_ = ProtocolV2::ErrorCode::CleanupFailed;
        }
        return cleaned;
    }

    bool cleanupStorage()
    {
        bool ok = true;
        if (storage_open_) {
            storage_open_ = false;
            if (!sink_.close || !sink_.close(sink_context_)) {
                ok = false;
            }
        }
        if (storage_present_) {
            if (!sink_.remove_partial || !sink_.remove_partial(sink_context_)) {
                ok = false;
            } else {
                storage_present_ = false;
            }
        }
        cleanup_failed_ = !ok;
        return ok;
    }

    void clearSession()
    {
        state_ = ReceiverState::Idle;
        error_ = ProtocolV2::ErrorCode::None;
        metadata_ = Metadata{};
        transfer_id_ = 0;
        expected_sequence_ = 0;
        accepted_packets_ = 0;
        accepted_bytes_ = 0;
        last_activity_ms_ = 0;
        crc_.reset();
        cleanup_failed_ = false;
        storage_open_ = false;
        storage_present_ = false;
    }

    bool makeReady(uint32_t transfer_id,
                   bool accepted,
                   uint32_t expected_sequence,
                   ProtocolV2::ErrorCode error,
                   ProtocolV2::Frame& response) const
    {
        ProtocolV2::Packet packet{};
        packet.type = ProtocolV2::PacketType::Ready;
        packet.transfer_id = transfer_id;
        packet.accepted = accepted;
        packet.expected_sequence = expected_sequence;
        packet.error = error;
        return ProtocolV2::encode(packet, response);
    }

    bool makeAck(uint16_t sequence, ProtocolV2::Frame& response) const
    {
        ProtocolV2::Packet packet{};
        packet.type = ProtocolV2::PacketType::Ack;
        packet.transfer_id = transfer_id_;
        packet.sequence = sequence;
        return ProtocolV2::encode(packet, response);
    }

    bool makeNack(uint32_t expected_sequence,
                  ProtocolV2::ErrorCode error,
                  ProtocolV2::Frame& response) const
    {
        ProtocolV2::Packet packet{};
        packet.type = ProtocolV2::PacketType::Nack;
        packet.transfer_id = transfer_id_;
        packet.expected_sequence = expected_sequence;
        packet.error = error;
        return ProtocolV2::encode(packet, response);
    }

    bool makeComplete(ProtocolV2::Frame& response) const
    {
        ProtocolV2::Packet packet{};
        packet.type = ProtocolV2::PacketType::Complete;
        packet.transfer_id = transfer_id_;
        packet.total_size = metadata_.total_size;
        packet.crc32 = metadata_.crc32;
        packet.error = ProtocolV2::ErrorCode::None;
        return ProtocolV2::encode(packet, response);
    }

    bool makeError(uint32_t transfer_id,
                   ProtocolV2::ErrorCode error,
                   uint32_t relevant_sequence,
                   ProtocolV2::Frame& response) const
    {
        ProtocolV2::Packet packet{};
        packet.type = ProtocolV2::PacketType::Error;
        packet.transfer_id = transfer_id;
        packet.error = error;
        packet.relevant_sequence = relevant_sequence;
        packet.state = static_cast<uint8_t>(state_);
        return ProtocolV2::encode(packet, response);
    }

    static bool metadataEqual(const Metadata& lhs, const Metadata& rhs)
    {
        return lhs.total_size == rhs.total_size &&
               lhs.total_packets == rhs.total_packets &&
               lhs.crc32 == rhs.crc32;
    }

    void* sink_context_ = nullptr;
    SinkCallbacks sink_{};
    uint32_t inactivity_timeout_ms_ = ProtocolV2::kReceiverInactivityTimeoutMs;
    ReceiverState state_ = ReceiverState::Idle;
    ProtocolV2::ErrorCode error_ = ProtocolV2::ErrorCode::None;
    Metadata metadata_{};
    uint32_t transfer_id_ = 0;
    uint32_t expected_sequence_ = 0;
    uint32_t accepted_packets_ = 0;
    uint32_t accepted_bytes_ = 0;
    uint64_t last_activity_ms_ = 0;
    ProtocolV2::Crc32 crc_{};
    bool storage_open_ = false;
    bool storage_present_ = false;
    bool cleanup_failed_ = false;
};

inline const char* senderStateName(SenderState state)
{
    switch (state) {
        case SenderState::Idle: return "Idle";
        case SenderState::Preparing: return "Preparing";
        case SenderState::WaitingForReady: return "WaitingForReady";
        case SenderState::SendingData: return "SendingData";
        case SenderState::WaitingForDataAck: return "WaitingForDataAck";
        case SenderState::WaitingForComplete: return "WaitingForComplete";
        case SenderState::Completed: return "Completed";
        case SenderState::Cancelling: return "Cancelling";
        case SenderState::Cancelled: return "Cancelled";
        case SenderState::Failed: return "Failed";
        default: return "Unknown";
    }
}

inline const char* receiverStateName(ReceiverState state)
{
    switch (state) {
        case ReceiverState::Idle: return "Idle";
        case ReceiverState::Receiving: return "Receiving";
        case ReceiverState::WaitingForEnd: return "WaitingForEnd";
        case ReceiverState::Verifying: return "Verifying";
        case ReceiverState::Publishing: return "Publishing";
        case ReceiverState::Completed: return "Completed";
        case ReceiverState::Cancelled: return "Cancelled";
        case ReceiverState::Failed: return "Failed";
        default: return "Unknown";
    }
}

}  // namespace ReliableTransferV2
