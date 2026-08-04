#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "reliable_transfer_v2.hpp"

// FileTransfer exposes a hardware-independent, subsystem-facing facade over
// the Protocol v2 sender state machine. It owns no radio, filesystem, task, or
// heap allocation. The caller keeps the selected IDataSource alive until the
// transfer reaches a terminal state and drives the transport adapter methods
// from its normal application task.
namespace FileTransfer {

using ReadResult = ReliableTransferV2::ReadResult;
using ReadState = ReliableTransferV2::ReadState;

constexpr size_t kLogicalNameCapacity = 64;
constexpr size_t kMediaTypeCapacity = 48;
constexpr size_t kPublishedPathCapacity = 96;
constexpr size_t kMemorySourceMaximumBytes = 4096;

enum class CollisionPolicy : uint8_t {
    CreateUnique,
    RejectIfExists,
    Replace,
};

enum class Role : uint8_t {
    Sender,
    Receiver,
};

enum class State : uint8_t {
    Idle,
    Preparing,
    Handshaking,
    Transferring,
    Verifying,
    Publishing,
    Completed,
    Cancelling,
    Cancelled,
    Failed,
};

enum class StartCode : uint8_t {
    Started,
    Busy,
    InvalidSource,
    ReadFailed,
    UnsupportedSize,
    LengthMismatch,
    SessionRejected,
};

template <size_t Capacity>
struct FixedText {
    std::array<char, Capacity> value{};
    bool present = false;

    bool set(std::string_view text)
    {
        value.fill('\0');
        present = !text.empty();
        if (!present) {
            return true;
        }
        if (text.size() >= Capacity) {
            present = false;
            return false;
        }
        std::memcpy(value.data(), text.data(), text.size());
        return true;
    }

    const char* c_str() const { return value.data(); }
};

struct TransferMetadata {
    FixedText<kLogicalNameCapacity> logical_filename{};
    FixedText<kMediaTypeCapacity> media_type{};
    uint32_t expected_length = 0;
    bool has_expected_length = false;
    CollisionPolicy collision_policy = CollisionPolicy::CreateUnique;
    void* user_context = nullptr;
};

struct TransferStatus {
    uint32_t transfer_id = 0;
    Role role = Role::Sender;
    State state = State::Idle;
    uint32_t bytes_transferred = 0;
    uint32_t total_bytes = 0;
    uint32_t current_sequence = 0;
    uint32_t total_packets = 0;
    uint32_t retry_count = 0;
    ProtocolV2::ErrorCode error = ProtocolV2::ErrorCode::None;
    uint32_t crc32 = 0;
    bool peer_complete = false;
    FixedText<kPublishedPathCapacity> final_published_path{};
    uint64_t started_ms = 0;
    uint64_t updated_ms = 0;
    uint64_t completed_ms = 0;
    uint64_t elapsed_ms = 0;
    TransferMetadata metadata{};
};

struct StartResult {
    StartCode code = StartCode::InvalidSource;
    uint32_t transfer_id = 0;

    explicit operator bool() const { return code == StartCode::Started; }
};

class IDataSource {
public:
    virtual ~IDataSource() = default;
    virtual bool reset() = 0;
    virtual ReadResult read(uint8_t* out, size_t capacity) = 0;
};

struct StreamCallbacks {
    bool (*reset)(void* context) = nullptr;
    ReadResult (*read)(void* context, uint8_t* out, size_t capacity) = nullptr;
};

// Adapts generated data, files, sockets, or other rewindable streams without
// buffering the complete object. The callbacks must distinguish EOF and error.
class StreamingDataSource final : public IDataSource {
public:
    StreamingDataSource(void* context, StreamCallbacks callbacks)
        : context_(context), callbacks_(callbacks)
    {
    }

    bool reset() override
    {
        return callbacks_.reset && callbacks_.reset(context_);
    }

    ReadResult read(uint8_t* out, size_t capacity) override
    {
        if (!callbacks_.read) {
            return {0, ReadState::Error};
        }
        return callbacks_.read(context_, out, capacity);
    }

private:
    void* context_ = nullptr;
    StreamCallbacks callbacks_{};
};

// BoundedMemoryDataSource references caller-owned memory. It deliberately
// rejects buffers larger than its explicit bound instead of allocating.
class BoundedMemoryDataSource final : public IDataSource {
public:
    BoundedMemoryDataSource(const uint8_t* data,
                            size_t size,
                            size_t maximum_size = kMemorySourceMaximumBytes)
        : data_(data), size_(size), maximum_size_(maximum_size)
    {
    }

    bool reset() override
    {
        offset_ = 0;
        return data_ != nullptr && size_ <= maximum_size_;
    }

    ReadResult read(uint8_t* out, size_t capacity) override
    {
        if (!out || capacity == 0 || !data_ || size_ > maximum_size_) {
            return {0, ReadState::Error};
        }
        if (offset_ == size_) {
            return {0, ReadState::EndOfFile};
        }
        const size_t remaining = size_ - offset_;
        const size_t count = remaining < capacity ? remaining : capacity;
        std::memcpy(out, data_ + offset_, count);
        offset_ += count;
        return {count, offset_ == size_ ? ReadState::EndOfFile
                                       : ReadState::MoreDataMayFollow};
    }

private:
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    size_t maximum_size_ = 0;
    size_t offset_ = 0;
};

struct ReceiveCompletion {
    TransferStatus status{};
};

using ReceiveHandler = void (*)(void* context, const ReceiveCompletion& completion);
using TransferIdSource = uint32_t (*)(void* context);

class Service {
public:
    explicit Service(TransferIdSource id_source = nullptr, void* id_context = nullptr)
        : id_source_(id_source),
          id_context_(id_context),
          sender_(this, sourceCallbacks())
    {
    }

    StartResult startTransfer(IDataSource& source,
                              const TransferMetadata& metadata,
                              uint64_t now_ms)
    {
        if (sender_.state() != ReliableTransferV2::SenderState::Idle &&
            !sender_.terminal()) {
            return {StartCode::Busy, sender_status_.transfer_id};
        }

        active_source_ = &source;
        sender_status_ = TransferStatus{};
        sender_status_.role = Role::Sender;
        sender_status_.state = State::Preparing;
        sender_status_.metadata = metadata;
        sender_status_.started_ms = now_ms;
        sender_status_.updated_ms = now_ms;

        ReliableTransferV2::Metadata wire_metadata{};
        const ReliableTransferV2::InspectResult inspected =
            ReliableTransferV2::inspectSource(this, sourceCallbacks(), wire_metadata);
        if (inspected != ReliableTransferV2::InspectResult::Ready) {
            active_source_ = nullptr;
            sender_status_.state = State::Failed;
            sender_status_.error = inspectError(inspected);
            sender_status_.updated_ms = now_ms;
            sender_status_.elapsed_ms = 0;
            return {inspectStartCode(inspected), 0};
        }
        if (metadata.has_expected_length &&
            metadata.expected_length != wire_metadata.total_size) {
            active_source_ = nullptr;
            sender_status_.state = State::Failed;
            sender_status_.error = ProtocolV2::ErrorCode::InvalidMetadata;
            return {StartCode::LengthMismatch, 0};
        }

        const uint32_t id = nextTransferId();
        sender_status_.transfer_id = id;
        if (!sender_.begin(id, wire_metadata, now_ms)) {
            sender_status_.state = State::Failed;
            sender_status_.error = sender_.error();
            active_source_ = nullptr;
            return {StartCode::SessionRejected, id};
        }
        refreshSenderStatus(now_ms);
        return {StartCode::Started, id};
    }

    bool cancelTransfer(uint32_t transfer_id, uint64_t now_ms)
    {
        if (transfer_id == 0 || transfer_id != sender_.transferId() || sender_.terminal()) {
            return false;
        }
        (void)sender_.cancel(now_ms);
        refreshSenderStatus(now_ms);
        return true;
    }

    bool getTransferStatus(uint32_t transfer_id,
                           TransferStatus& out,
                           uint64_t now_ms) const
    {
        if (transfer_id != 0 && transfer_id == sender_status_.transfer_id) {
            out = sender_status_;
        } else if (transfer_id != 0 && transfer_id == receiver_status_.transfer_id) {
            out = receiver_status_;
        } else {
            return false;
        }
        const uint64_t end = out.completed_ms != 0 ? out.completed_ms : now_ms;
        out.elapsed_ms = end >= out.started_ms ? end - out.started_ms : 0;
        return true;
    }

    void registerReceiveHandler(ReceiveHandler handler, void* context)
    {
        receive_handler_ = handler;
        receive_handler_context_ = context;
    }

    // Transport adapter. These calls preserve the existing Protocol v2 wire
    // state machine and should be driven by one application task.
    ReliableTransferV2::SenderSession& transportSender() { return sender_; }
    const ReliableTransferV2::SenderSession& transportSender() const { return sender_; }

    void refreshSenderStatus(uint64_t now_ms)
    {
        sender_status_.transfer_id = sender_.transferId();
        sender_status_.state = mapSenderState(sender_.state());
        sender_status_.bytes_transferred = sender_.acknowledgedBytes();
        sender_status_.total_bytes = sender_.totalSize();
        sender_status_.current_sequence = sender_.currentSequence();
        sender_status_.total_packets = sender_.totalPackets();
        sender_status_.retry_count = sender_.totalRetries();
        sender_status_.error = sender_.error();
        sender_status_.crc32 = sender_.crc32();
        sender_status_.peer_complete = sender_.peerComplete();
        sender_status_.updated_ms = now_ms;
        if (sender_.terminal()) {
            if (sender_status_.completed_ms == 0) {
                sender_status_.completed_ms = now_ms;
            }
            active_source_ = nullptr;
        }
        sender_status_.elapsed_ms =
            now_ms >= sender_status_.started_ms ? now_ms - sender_status_.started_ms : 0;
    }

    void reportReceiveStarted(uint32_t transfer_id,
                              uint32_t total_bytes,
                              uint32_t total_packets,
                              uint32_t crc32,
                              uint64_t now_ms)
    {
        receiver_status_ = TransferStatus{};
        receiver_status_.transfer_id = transfer_id;
        receiver_status_.role = Role::Receiver;
        receiver_status_.state = State::Handshaking;
        receiver_status_.total_bytes = total_bytes;
        receiver_status_.total_packets = total_packets;
        receiver_status_.crc32 = crc32;
        receiver_status_.started_ms = now_ms;
        receiver_status_.updated_ms = now_ms;
        receiver_status_.metadata.has_expected_length = true;
        receiver_status_.metadata.expected_length = total_bytes;
    }

    void reportReceiveProgress(uint32_t transfer_id,
                               uint32_t bytes,
                               uint32_t sequence,
                               uint64_t now_ms)
    {
        if (transfer_id == 0 || transfer_id != receiver_status_.transfer_id) {
            return;
        }
        receiver_status_.state = State::Transferring;
        receiver_status_.bytes_transferred = bytes;
        receiver_status_.current_sequence = sequence;
        receiver_status_.updated_ms = now_ms;
        receiver_status_.elapsed_ms = now_ms >= receiver_status_.started_ms
                                          ? now_ms - receiver_status_.started_ms : 0;
    }

    // Call only after the receiver has verified CRC/size and atomically
    // published the completed file. The handler runs synchronously in the
    // caller's task context; it is never suitable for an ISR.
    void reportReceiveCompleted(uint32_t transfer_id,
                                std::string_view published_path,
                                uint64_t now_ms)
    {
        if (transfer_id == 0 || transfer_id != receiver_status_.transfer_id) {
            return;
        }
        receiver_status_.state = State::Completed;
        receiver_status_.bytes_transferred = receiver_status_.total_bytes;
        receiver_status_.current_sequence = receiver_status_.total_packets == 0
                                                ? 0
                                                : receiver_status_.total_packets - 1;
        receiver_status_.error = ProtocolV2::ErrorCode::None;
        receiver_status_.peer_complete = true;
        (void)receiver_status_.final_published_path.set(published_path);
        receiver_status_.updated_ms = now_ms;
        receiver_status_.completed_ms = now_ms;
        receiver_status_.elapsed_ms = now_ms >= receiver_status_.started_ms
                                          ? now_ms - receiver_status_.started_ms : 0;
        if (receive_handler_) {
            receive_handler_(receive_handler_context_, ReceiveCompletion{receiver_status_});
        }
    }

    void reportReceiveFailed(uint32_t transfer_id,
                             ProtocolV2::ErrorCode error,
                             uint64_t now_ms)
    {
        if (transfer_id == 0 || transfer_id != receiver_status_.transfer_id) {
            return;
        }
        receiver_status_.state = error == ProtocolV2::ErrorCode::Cancelled
                                     ? State::Cancelled : State::Failed;
        receiver_status_.error = error;
        receiver_status_.updated_ms = now_ms;
        receiver_status_.completed_ms = now_ms;
    }

private:
    static bool resetSource(void* context)
    {
        Service* service = static_cast<Service*>(context);
        return service && service->active_source_ && service->active_source_->reset();
    }

    static ReadResult readSource(void* context, uint8_t* out, size_t capacity)
    {
        Service* service = static_cast<Service*>(context);
        if (!service || !service->active_source_) {
            return {0, ReadState::Error};
        }
        return service->active_source_->read(out, capacity);
    }

    static ReliableTransferV2::SourceCallbacks sourceCallbacks()
    {
        return {&Service::resetSource, &Service::readSource};
    }

    uint32_t nextTransferId()
    {
        uint32_t candidate = id_source_ ? id_source_(id_context_) : ++fallback_id_;
        if (candidate == 0) {
            candidate = ++fallback_id_;
        }
        if (candidate == last_transfer_id_) {
            ++candidate;
            if (candidate == 0) {
                candidate = 1;
            }
        }
        last_transfer_id_ = candidate;
        return candidate;
    }

    static StartCode inspectStartCode(ReliableTransferV2::InspectResult result)
    {
        switch (result) {
            case ReliableTransferV2::InspectResult::UnsupportedSize:
                return StartCode::UnsupportedSize;
            case ReliableTransferV2::InspectResult::ReadFailed:
            case ReliableTransferV2::InspectResult::ResetFailed:
                return StartCode::ReadFailed;
            case ReliableTransferV2::InspectResult::InvalidCallbacks:
                return StartCode::InvalidSource;
            case ReliableTransferV2::InspectResult::Ready:
            default:
                return StartCode::SessionRejected;
        }
    }

    static ProtocolV2::ErrorCode inspectError(ReliableTransferV2::InspectResult result)
    {
        return result == ReliableTransferV2::InspectResult::UnsupportedSize
                   ? ProtocolV2::ErrorCode::UnsupportedSize
                   : ProtocolV2::ErrorCode::SourceRead;
    }

    static State mapSenderState(ReliableTransferV2::SenderState state)
    {
        using SenderState = ReliableTransferV2::SenderState;
        switch (state) {
            case SenderState::Preparing: return State::Preparing;
            case SenderState::WaitingForReady: return State::Handshaking;
            case SenderState::SendingData:
            case SenderState::WaitingForDataAck: return State::Transferring;
            case SenderState::WaitingForComplete: return State::Verifying;
            case SenderState::Completed: return State::Completed;
            case SenderState::Cancelling: return State::Cancelling;
            case SenderState::Cancelled: return State::Cancelled;
            case SenderState::Failed: return State::Failed;
            case SenderState::Idle:
            default: return State::Idle;
        }
    }

    TransferIdSource id_source_ = nullptr;
    void* id_context_ = nullptr;
    uint32_t fallback_id_ = 0;
    uint32_t last_transfer_id_ = 0;
    IDataSource* active_source_ = nullptr;
    ReliableTransferV2::SenderSession sender_;
    TransferStatus sender_status_{};
    TransferStatus receiver_status_{};
    ReceiveHandler receive_handler_ = nullptr;
    void* receive_handler_context_ = nullptr;
};

}  // namespace FileTransfer
