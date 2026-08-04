#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "audio_packet.hpp"

// FileReceiver owns the hardware-independent safety rules for one streamed
// file receive session. Filesystem work is supplied through fixed callbacks so
// the same state machine can run against SPIFFS in firmware and a fake sink in
// native tests.
namespace FileReceiver {

constexpr uint64_t kDefaultInactivityTimeoutMs = 10000;

enum class State {
    Idle,
    AwaitingFirst,
    Receiving,
    Completed,
    Failed,
};

enum class Failure {
    None,
    CleanupFailed,
    OpenFailed,
    InvalidPacket,
    MissingFirst,
    UnexpectedFirst,
    SequenceGap,
    SequenceLimit,
    WriteFailed,
    CloseFailed,
    PublicationFailed,
    Timeout,
    Cancelled,
    StateViolation,
};

enum class Result {
    Started,
    StartRejectedActive,
    Accepted,
    Duplicate,
    Completed,
    IgnoredStop,
    Stopped,
    NoActiveSession,
    AlreadyComplete,
    Aborted,
    TimedOut,
};

enum class PrepareResult {
    Ready,
    CleanupFailed,
    OpenFailed,
};

struct StorageCallbacks {
    // Prepare a new partial destination for stream_id and leave it open.
    PrepareResult (*prepare)(void* context, uint16_t stream_id) = nullptr;
    // Return the number of payload bytes successfully written.
    size_t (*write)(void* context, const uint8_t* data, size_t length) = nullptr;
    // Close consumes the open handle even when it reports a flush/close error.
    bool (*close)(void* context) = nullptr;
    // Publish atomically moves the closed partial destination to its final name.
    bool (*publish)(void* context) = nullptr;
    // Remove the internal partial destination. Repeated calls must be safe.
    bool (*remove_partial)(void* context) = nullptr;
};

struct Packet {
    uint16_t sequence = 0;
    uint8_t payload_length = 0;
    uint8_t flags = 0;
};

class Transfer {
public:
    Transfer(void* storage_context,
             StorageCallbacks storage,
             uint64_t timeout_ms = kDefaultInactivityTimeoutMs)
        : storage_context_(storage_context),
          storage_(storage),
          timeout_ms_(timeout_ms)
    {
    }

    Result start(uint16_t stream_id, uint64_t now_ms)
    {
        if (active()) {
            return Result::StartRejectedActive;
        }

        if (storage_open_ || storage_present_) {
            if (!cleanupStorage()) {
                state_ = State::Failed;
                failure_ = Failure::CleanupFailed;
                return Result::Aborted;
            }
        }

        clearSessionMetadata();
        if (!callbacksValid()) {
            state_ = State::Failed;
            failure_ = Failure::StateViolation;
            return Result::Aborted;
        }

        const PrepareResult prepared = storage_.prepare(storage_context_, stream_id);
        if (prepared != PrepareResult::Ready) {
            state_ = State::Failed;
            failure_ = prepared == PrepareResult::CleanupFailed
                           ? Failure::CleanupFailed
                           : Failure::OpenFailed;
            storage_present_ = prepared == PrepareResult::CleanupFailed;
            return Result::Aborted;
        }

        storage_open_ = true;
        storage_present_ = true;
        state_ = State::AwaitingFirst;
        stream_id_ = stream_id;
        last_activity_ms_ = now_ms;
        return Result::Started;
    }

    Result accept(const Packet& packet, const uint8_t* payload, uint64_t now_ms)
    {
        last_received_sequence_ = packet.sequence;

        if (state_ == State::Completed) {
            return Result::AlreadyComplete;
        }
        if (!active()) {
            return Result::NoActiveSession;
        }

        constexpr uint8_t kKnownFlags = AudioPacket::kFirst | AudioPacket::kLast;
        if (!payload || packet.payload_length == 0 ||
            packet.payload_length > AudioPacket::kAudioBytesPerPacket ||
            (packet.flags & static_cast<uint8_t>(~kKnownFlags)) != 0) {
            return fail(Failure::InvalidPacket);
        }

        const bool first = (packet.flags & AudioPacket::kFirst) != 0;
        const bool last = (packet.flags & AudioPacket::kLast) != 0;

        if (state_ == State::AwaitingFirst) {
            if (packet.sequence != 0 || !first) {
                return fail(Failure::MissingFirst);
            }
        } else {
            if (static_cast<uint32_t>(packet.sequence) < expected_sequence_) {
                return Result::Duplicate;
            }
            if (static_cast<uint32_t>(packet.sequence) > expected_sequence_) {
                return fail(Failure::SequenceGap);
            }
            if (first) {
                return fail(Failure::UnexpectedFirst);
            }
        }

        if (packet.sequence == UINT16_MAX && !last) {
            return fail(Failure::SequenceLimit);
        }

        const size_t written = storage_.write(
            storage_context_, payload, packet.payload_length);
        if (written != packet.payload_length) {
            return fail(Failure::WriteFailed);
        }

        ++accepted_packets_;
        accepted_bytes_ += packet.payload_length;
        expected_sequence_ = static_cast<uint32_t>(packet.sequence) + 1u;
        last_activity_ms_ = now_ms;

        if (!last) {
            state_ = State::Receiving;
            return Result::Accepted;
        }

        storage_open_ = false;
        if (!storage_.close(storage_context_)) {
            return fail(Failure::CloseFailed);
        }
        if (!storage_.publish(storage_context_)) {
            return fail(Failure::PublicationFailed);
        }

        storage_present_ = false;
        state_ = State::Completed;
        failure_ = Failure::None;
        return Result::Completed;
    }

    Result rejectMalformedPacket()
    {
        if (state_ == State::Completed) {
            return Result::AlreadyComplete;
        }
        if (!active()) {
            return Result::NoActiveSession;
        }
        return fail(Failure::InvalidPacket);
    }

    Result stop(uint16_t stream_id)
    {
        if (!active()) {
            return state_ == State::Completed ? Result::AlreadyComplete
                                               : Result::NoActiveSession;
        }
        if (stream_id != stream_id_) {
            return Result::IgnoredStop;
        }

        (void)abort(Failure::Cancelled);
        return Result::Stopped;
    }

    Result checkTimeout(uint64_t now_ms)
    {
        if (!active()) {
            return Result::NoActiveSession;
        }
        if (now_ms < last_activity_ms_ || now_ms - last_activity_ms_ < timeout_ms_) {
            return Result::Accepted;
        }

        (void)fail(Failure::Timeout);
        return Result::TimedOut;
    }

    Result abort(Failure failure)
    {
        if (state_ == State::Completed) {
            return Result::AlreadyComplete;
        }
        if (!active() && state_ != State::Failed) {
            return Result::NoActiveSession;
        }

        if (active()) {
            state_ = State::Failed;
            failure_ = failure == Failure::None ? Failure::StateViolation : failure;
        }
        (void)cleanupStorage();
        return Result::Aborted;
    }

    bool reset()
    {
        const bool cleaned = cleanupStorage();
        clearSessionMetadata();
        if (!cleaned) {
            state_ = State::Failed;
            failure_ = Failure::CleanupFailed;
            return false;
        }
        return true;
    }

    State state() const { return state_; }
    Failure failure() const { return failure_; }
    bool active() const
    {
        return state_ == State::AwaitingFirst || state_ == State::Receiving;
    }
    bool complete() const { return state_ == State::Completed; }
    bool cleanupFailed() const { return cleanup_failed_; }
    uint16_t streamId() const { return stream_id_; }
    uint32_t expectedSequence() const { return expected_sequence_; }
    uint16_t lastReceivedSequence() const { return last_received_sequence_; }
    uint32_t acceptedPackets() const { return accepted_packets_; }
    uint32_t acceptedBytes() const { return accepted_bytes_; }
    uint64_t lastActivityMs() const { return last_activity_ms_; }
    uint64_t timeoutMs() const { return timeout_ms_; }

private:
    bool callbacksValid() const
    {
        return storage_.prepare && storage_.write && storage_.close &&
               storage_.publish && storage_.remove_partial;
    }

    Result fail(Failure failure)
    {
        state_ = State::Failed;
        failure_ = failure;
        (void)cleanupStorage();
        return Result::Aborted;
    }

    bool cleanupStorage()
    {
        bool ok = true;
        if (storage_open_) {
            storage_open_ = false;
            if (!storage_.close || !storage_.close(storage_context_)) {
                ok = false;
            }
        }
        if (storage_present_) {
            if (!storage_.remove_partial || !storage_.remove_partial(storage_context_)) {
                ok = false;
            } else {
                storage_present_ = false;
            }
        }
        cleanup_failed_ = !ok;
        return ok;
    }

    void clearSessionMetadata()
    {
        state_ = State::Idle;
        failure_ = Failure::None;
        stream_id_ = 0;
        expected_sequence_ = 0;
        last_received_sequence_ = 0;
        accepted_packets_ = 0;
        accepted_bytes_ = 0;
        last_activity_ms_ = 0;
        cleanup_failed_ = false;
    }

    void* storage_context_ = nullptr;
    StorageCallbacks storage_{};
    uint64_t timeout_ms_ = kDefaultInactivityTimeoutMs;
    State state_ = State::Idle;
    Failure failure_ = Failure::None;
    uint16_t stream_id_ = 0;
    uint32_t expected_sequence_ = 0;
    uint16_t last_received_sequence_ = 0;
    uint32_t accepted_packets_ = 0;
    uint32_t accepted_bytes_ = 0;
    uint64_t last_activity_ms_ = 0;
    bool storage_open_ = false;
    bool storage_present_ = false;
    bool cleanup_failed_ = false;
};

inline bool isInternalTransferName(std::string_view name)
{
    constexpr std::string_view kPartialSuffix = ".part";
    return name.size() >= kPartialSuffix.size() &&
           name.substr(name.size() - kPartialSuffix.size()) == kPartialSuffix;
}

inline const char* failureName(Failure failure)
{
    switch (failure) {
        case Failure::None: return "none";
        case Failure::CleanupFailed: return "cleanup-failed";
        case Failure::OpenFailed: return "open-failed";
        case Failure::InvalidPacket: return "invalid-packet";
        case Failure::MissingFirst: return "invalid-first-packet";
        case Failure::UnexpectedFirst: return "repeated-first";
        case Failure::SequenceGap: return "sequence-gap";
        case Failure::SequenceLimit: return "sequence-limit";
        case Failure::WriteFailed: return "write-failed";
        case Failure::CloseFailed: return "close-failed";
        case Failure::PublicationFailed: return "publication-failed";
        case Failure::Timeout: return "timeout";
        case Failure::Cancelled: return "cancelled";
        case Failure::StateViolation: return "state-violation";
        default: return "unknown";
    }
}

}  // namespace FileReceiver
