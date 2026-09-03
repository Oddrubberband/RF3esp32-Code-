// Software-only qualification: production V2 service/sessions, fake frames,
// and host filesystem callbacks. No ESP-IDF, SPI, nRF24, or RF is exercised.
#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "file_transfer_service.hpp"
#include "protocol_v2_fake_transport.hpp"

namespace fs = std::filesystem;
using namespace ProtocolV2;
using namespace ReliableTransferV2;
using namespace ProtocolV2Test;

namespace {
constexpr uint32_t kSeed = 0x52463332u;
constexpr uint32_t kSongBytes = 1112701;
constexpr uint32_t kStepMs = 10;

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

// Unsigned, defined modulo-2^32 arithmetic. Address-dependent data does not
// repeat every 256 bytes, and a new seed changes every transfer's oracle.
uint8_t pattern(uint32_t index, uint32_t seed)
{
    uint32_t x = (index ^ seed) + 0x9E3779B9u;
    x = (x ^ (x >> 16)) * 0x85EBCA6Bu;
    x = (x ^ (x >> 13)) * 0xC2B2AE35u;
    return static_cast<uint8_t>((x ^ (x >> 16)) & 0xFFu);
}

uint32_t nextRandom(uint32_t& x)
{
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

struct Source {
    fs::path path;
    uint32_t size;
    uint32_t seed;
    size_t chunk;
    std::FILE* file = nullptr;
    uint32_t offset = 0;
    uint64_t resets = 0;
    uint64_t bytes_read = 0;

    Source(uint32_t length, uint32_t salt, size_t maximum_chunk,
           const fs::path& input = {})
        : path(input), size(length), seed(salt), chunk(maximum_chunk)
    {
        if (!path.empty()) {
            require(fs::file_size(path) == size, "source size differs from qualification baseline");
            file = std::fopen(path.string().c_str(), "rb");
            require(file != nullptr, "cannot open source");
        }
    }
    ~Source() { if (file) std::fclose(file); }
    Source(const Source&) = delete;
    Source& operator=(const Source&) = delete;

    static bool reset(void* context)
    {
        auto& s = *static_cast<Source*>(context);
        s.offset = 0;
        ++s.resets;
        if (s.file) {
            std::clearerr(s.file);
            return std::fseek(s.file, 0, SEEK_SET) == 0;
        }
        return true;
    }
    static ReadResult read(void* context, uint8_t* out, size_t capacity)
    {
        auto& s = *static_cast<Source*>(context);
        const size_t count = std::min({capacity, s.chunk, size_t(s.size - s.offset)});
        if (s.file) {
            if (std::fread(out, 1, count, s.file) != count) return {0, ReadState::Error};
        } else {
            for (size_t i = 0; i < count; ++i) out[i] = pattern(s.offset + uint32_t(i), s.seed);
        }
        s.offset += uint32_t(count);
        s.bytes_read += count;
        return {count, s.offset == s.size ? ReadState::EndOfFile : ReadState::MoreDataMayFollow};
    }
};

struct Audit {
    uint64_t bytes = 0;
    uint64_t errors = 0;
    uint32_t crc = 0;
};

Audit auditFile(const fs::path& path, const Source& source)
{
    std::ifstream input(path, std::ios::binary);
    require(bool(input), "cannot read persisted output: " + path.string());
    std::ifstream oracle;
    if (!source.path.empty()) {
        oracle.open(source.path, std::ios::binary);
        require(bool(oracle), "cannot open independent source comparison stream");
    }
    std::array<uint8_t, 4096> data{}, expected{};
    Crc32 crc;
    Audit result;
    while (input) {
        input.read(reinterpret_cast<char*>(data.data()), data.size());
        const size_t count = size_t(input.gcount());
        if (oracle.is_open()) {
            oracle.read(reinterpret_cast<char*>(expected.data()), count);
        } else {
            for (size_t i = 0; i < count; ++i)
                expected[i] = pattern(uint32_t(result.bytes + i), source.seed);
        }
        const size_t comparable = oracle.is_open() ? size_t(oracle.gcount()) : count;
        for (size_t i = 0; i < count; ++i) {
            if (result.bytes + i >= source.size || i >= comparable || data[i] != expected[i])
                ++result.errors;
        }
        crc.update(data.data(), count);
        result.bytes += count;
    }
    require(input.eof(), "persisted output read failed");
    if (oracle.is_open()) require(!oracle.bad(), "source comparison read failed");
    if (result.bytes < source.size) result.errors += source.size - result.bytes;
    result.crc = crc.value();
    return result;
}

struct Sink {
    fs::path directory, partial, final;
    Source* source = nullptr;
    ReceiverSession* receiver = nullptr;
    std::FILE* file = nullptr;
    uint64_t prepares = 0, writes = 0, bytes = 0, closes = 0;
    uint64_t publish_calls = 0, publishes = 0, removes = 0, invalid_publishes = 0;
    bool saw_partial = false, audited = false;
    Audit audit;

    explicit Sink(const fs::path& root) : directory(root) {}
    ~Sink() { if (file) std::fclose(file); }

    void select(Source& input)
    {
        require(file == nullptr && (partial.empty() || !fs::exists(partial)), "previous partial survived");
        source = &input;
        partial.clear();
        final.clear();
        prepares = writes = bytes = closes = publish_calls = publishes = removes = invalid_publishes = 0;
        saw_partial = audited = false;
        audit = {};
    }
    static SinkPrepareResult prepare(void* context, uint32_t id, uint32_t)
    {
        auto& s = *static_cast<Sink*>(context);
        ++s.prepares;
        s.final = s.directory / ("rx_" + std::to_string(id) + ".bin");
        s.partial = s.final.string() + ".part";
        if (fs::exists(s.final) || fs::exists(s.partial) || s.file) return SinkPrepareResult::OpenFailed;
        s.file = std::fopen(s.partial.string().c_str(), "wb");
        s.saw_partial = s.file && fs::exists(s.partial) && !fs::exists(s.final);
        return s.file ? SinkPrepareResult::Ready : SinkPrepareResult::OpenFailed;
    }
    static size_t write(void* context, const uint8_t* data, size_t length)
    {
        auto& s = *static_cast<Sink*>(context);
        ++s.writes;
        if (!s.file) return 0;
        const size_t written = std::fwrite(data, 1, length, s.file);
        s.bytes += written;
        return written;
    }
    static bool close(void* context)
    {
        auto& s = *static_cast<Sink*>(context);
        ++s.closes;
        if (!s.file) return false;
        const bool flushed = std::fflush(s.file) == 0;
        const bool closed = std::fclose(s.file) == 0;
        s.file = nullptr;
        if (flushed && closed) {
            s.audit = auditFile(s.partial, *s.source);
            s.audited = true;
        }
        return flushed && closed;
    }
    static bool publish(void* context)
    {
        auto& s = *static_cast<Sink*>(context);
        ++s.publish_calls;
        // Observe violations, but DO NOT protect production from them by
        // refusing to publish on an oracle/CRC mismatch. The session must gate it.
        const bool valid = s.receiver->state() == ReceiverState::Publishing &&
            s.receiver->acceptedBytes() == s.source->size &&
            s.receiver->acceptedPackets() == (s.source->size + 19u) / 20u &&
            s.receiver->calculatedCrc32() == s.receiver->expectedCrc32() &&
            s.audited && s.audit.bytes == s.source->size && s.audit.errors == 0 &&
            s.audit.crc == s.receiver->expectedCrc32() && !s.file;
        if (!valid) ++s.invalid_publishes;
        if (s.file || fs::exists(s.final)) return false;
        if (std::rename(s.partial.string().c_str(), s.final.string().c_str()) != 0) return false;
        ++s.publishes;
        return true;
    }
    static bool remove(void* context)
    {
        auto& s = *static_cast<Sink*>(context);
        ++s.removes;
        if (s.file) return false;  // Production must close before cleanup.
        return fs::remove(s.partial);
    }
    static SinkCallbacks callbacks() { return {prepare, write, close, publish, remove}; }
};

std::string quoted(const std::string& value)
{
    std::string result = "\"";
    for (char c : value) {
        if (c == '\\' || c == '"') result += '\\';
        if (c == '\n') result += "\\n";
        else if (c == '\r') result += "\\r";
        else result += c;
    }
    return result + "\"";
}

struct Record {
    std::map<std::string, uint64_t> n;
    std::vector<std::string> failures;
    std::string category, name, sender_state, receiver_state, output;
    std::vector<FaultRule> rules;
    std::vector<uint32_t> applications;
    void check(bool ok, const std::string& message) { if (!ok) failures.push_back(message); }
    void emit(std::ostream& out) const
    {
        out << "{\"category\":" << quoted(category) << ",\"name\":" << quoted(name)
            << ",\"sender_state\":" << quoted(sender_state) << ",\"receiver_state\":" << quoted(receiver_state)
            << ",\"output\":" << quoted(output) << ",\"passed\":" << (failures.empty() ? "true" : "false");
        for (const auto& item : n) out << ',' << quoted(item.first) << ':' << item.second;
        out << ",\"failures\":[";
        for (size_t i = 0; i < failures.size(); ++i) out << (i ? "," : "") << quoted(failures[i]);
        out << "],\"faults\":[";
        for (size_t i = 0; i < rules.size(); ++i) {
            const auto& r = rules[i];
            out << (i ? "," : "") << "{\"type\":" << unsigned(r.type)
                << ",\"kind\":" << unsigned(r.kind) << ",\"destination\":" << unsigned(r.destination)
                << ",\"sequence\":" << r.sequence << ",\"any_sequence\":" << (r.any_sequence ? "true" : "false")
                << ",\"occurrence\":" << r.occurrence << ",\"repeat\":" << r.repeat_count
                << ",\"offset\":" << r.corrupt_offset << ",\"mask\":" << unsigned(r.corrupt_mask)
                << ",\"applied\":" << applications[i] << '}';
        }
        out << "]}\n";
        out.flush();
        require(bool(out), "cannot write qualification measurements");
    }
};

struct Harness {
    Sink sink;
    FileTransfer::Service service;
    ReceiverSession receiver;
    uint64_t now = 0;
    Frame previous_data{}, previous_ack{};
    bool have_previous = false;
    explicit Harness(const fs::path& directory)
        : sink(directory), receiver(&sink, Sink::callbacks())
    {
        fs::create_directories(directory);
        sink.receiver = &receiver;
    }

    Record run(Source& source, const std::string& category, const std::string& name,
               const std::vector<FaultRule>& rules = {}, bool corrupt = false, bool stale_probe = false)
    {
        Record r;
        r.category = category;
        r.name = name;
        r.rules = rules;
        auto& m = r.n;
        m = {{"seed", source.seed}, {"expected_bytes", source.size},
             {"expected_packets", (source.size + 19u) / 20u}, {"chunk", source.chunk},
             {"expected_rejection", corrupt}, {"tx_frames", 0}, {"tx_data_packets", 0},
             {"tx_data_bytes", 0}, {"rx_data_frames", 0}, {"rx_data_bytes", 0},
             {"accepted_events", 0}, {"duplicate_data", 0}, {"sequence_errors", 0},
             {"early_final_files", 0}, {"stale_frames_ignored", 0},
             {"drops", 0}, {"corruptions", 0}, {"max_retry_burst", 0}};
        sink.select(source);
        FakeDuplexTransport transport(source.seed);
        for (const auto& rule : rules) require(transport.addRule(rule), "fault rule capacity exceeded");
        FileTransfer::StreamingDataSource stream(&source, {Source::reset, Source::read});
        FileTransfer::TransferMetadata metadata;
        metadata.has_expected_length = true;
        metadata.expected_length = source.size;
        const auto start = service.startTransfer(stream, metadata, now);
        require(bool(start), "production service refused source: " + name);
        auto& sender = service.transportSender();
        r.check(sender.currentSequence() == 0 && sender.acknowledgedBytes() == 0 &&
                sender.acknowledgedPackets() == 0 && sender.totalRetries() == 0 &&
                !sender.peerComplete() && sender.error() == ErrorCode::None, "sender reset counters");
        const uint64_t started = now;
        bool probed = false;
        Frame latest_data{}, latest_ack{};
        bool saw_data = false;
        uint64_t steps = 0;
        for (; steps < 300000 && !sender.terminal(); ++steps, now += kStepMs) {
            Frame frame{};
            if (sender.outboundFrame(frame)) {
                Packet packet{};
                require(decode(frame.data(), frame.size(), packet) == DecodeStatus::Ok, "sender emitted invalid frame");
                ++m["tx_frames"];
                if (packet.type == PacketType::Data) {
                    ++m["tx_data_packets"];
                    m["tx_data_bytes"] += packet.payload_length;
                    latest_data = frame;
                    saw_data = true;
                    m["last_tx_sequence"] = packet.sequence;
                    m["last_payload_bytes"] = packet.payload_length;
                }
                require(transport.send(Destination::Receiver, frame, now), "fake transport queue overflow");
                require(sender.noteFrameSent(now), "sender did not accept send notification");
            }
            while (transport.pop(Destination::Receiver, now, frame)) {
                Packet packet{};
                require(decode(frame.data(), frame.size(), packet) == DecodeStatus::Ok, "unexpected malformed injected frame");
                const uint32_t accepted_before = receiver.acceptedPackets();
                Frame response{};
                const auto event = receiver.onFrame(frame.data(), frame.size(), now, response);
                if (packet.type == PacketType::Start && event == ReceiverEvent::ResponseReady) {
                    r.check(receiver.acceptedBytes() == 0 && receiver.acceptedPackets() == 0 &&
                            receiver.expectedSequence() == 0 && receiver.calculatedCrc32() == 0 &&
                            receiver.transferId() == start.transfer_id, "receiver START reset counters/CRC/id");
                    if (stale_probe && have_previous && !probed) {
                        Frame ignored{};
                        if (receiver.onFrame(previous_data.data(), previous_data.size(), now, ignored) == ReceiverEvent::Ignored)
                            ++m["stale_frames_ignored"];
                        if (sender.onFrame(previous_ack.data(), previous_ack.size(), now) == SenderEvent::Ignored)
                            ++m["stale_frames_ignored"];
                        r.check(receiver.acceptedPackets() == 0 && receiver.acceptedBytes() == 0 &&
                                sender.acknowledgedPackets() == 0 && sender.acknowledgedBytes() == 0,
                                "stale prior-transfer frames changed counters");
                        probed = true;
                    }
                }
                if (packet.type == PacketType::Data) {
                    ++m["rx_data_frames"];
                    m["rx_data_bytes"] += packet.payload_length;
                    if (event == ReceiverEvent::Duplicate) ++m["duplicate_data"];
                    if (event == ReceiverEvent::DataAccepted) {
                        if (packet.sequence != accepted_before ||
                            packet.payload_length != std::min<uint32_t>(20, source.size - accepted_before * 20))
                            ++m["sequence_errors"];
                        ++m["accepted_events"];
                        m["last_accepted_sequence"] = packet.sequence;
                        latest_ack = response;
                    }
                }
                if (event != ReceiverEvent::Ignored)
                    require(transport.send(Destination::Sender, response, now), "response queue overflow");
            }
            while (transport.pop(Destination::Sender, now, frame))
                (void)sender.onFrame(frame.data(), frame.size(), now);
            Frame timeout{};
            if (receiver.tick(now, timeout) == ReceiverEvent::TimedOut)
                require(transport.send(Destination::Sender, timeout, now), "timeout queue overflow");
            (void)sender.tick(now);
            m["max_retry_burst"] = std::max<uint64_t>(m["max_retry_burst"], sender.retryCount());
            if (!sink.final.empty() && fs::exists(sink.final) && receiver.state() != ReceiverState::Completed)
                ++m["early_final_files"];
        }
        service.refreshSenderStatus(now);
        FileTransfer::TransferStatus status;
        require(service.getTransferStatus(start.transfer_id, status, now), "service status missing");
        m["transfer_id"] = start.transfer_id;
        m["steps"] = steps;
        m["simulated_ms"] = now - started;
        m["source_resets"] = source.resets;
        m["source_bytes_read"] = source.bytes_read;
        m["acked_bytes"] = sender.acknowledgedBytes();
        m["acked_packets"] = sender.acknowledgedPackets();
        m["accepted_bytes"] = receiver.acceptedBytes();
        m["accepted_packets"] = receiver.acceptedPackets();
        m["sender_next_sequence"] = sender.currentSequence();
        m["receiver_next_sequence"] = receiver.expectedSequence();
        m["source_crc32"] = sender.crc32();
        m["receiver_crc32"] = receiver.calculatedCrc32();
        m["persisted_crc32"] = sink.audit.crc;
        m["persisted_bytes"] = sink.audit.bytes;
        m["byte_errors"] = sink.audit.errors;
        m["retries"] = sender.totalRetries();
        m["sender_error"] = unsigned(sender.error());
        m["receiver_error"] = unsigned(receiver.error());
        m["sink_bytes"] = sink.bytes;
        m["sink_writes"] = sink.writes;
        m["sink_prepares"] = sink.prepares;
        m["sink_closes"] = sink.closes;
        m["publish_calls"] = sink.publish_calls;
        m["publishes"] = sink.publishes;
        m["cleanup_calls"] = sink.removes;
        m["invalid_publishes"] = sink.invalid_publishes;
        m["partial_remaining"] = !sink.partial.empty() && fs::exists(sink.partial);
        m["final_exists"] = !sink.final.empty() && fs::exists(sink.final);
        r.sender_state = senderStateName(sender.state());
        r.receiver_state = receiverStateName(receiver.state());
        // Failed/corrupted transfers intentionally have no final path on disk.
        r.output = sink.final.lexically_relative(sink.directory.parent_path()).generic_string();
        uint64_t dropped_data = 0, dropped_data_bytes = 0, dropped_acks = 0, changed_bytes = 0;
        for (size_t i = 0; i < rules.size(); ++i) {
            const uint32_t applied = transport.ruleApplications(i);
            r.applications.push_back(applied);
            r.check(applied == rules[i].repeat_count, "configured fault did not execute exactly");
            if (rules[i].kind == FaultKind::Drop) {
                m["drops"] += applied;
                if (rules[i].type == PacketType::Data) {
                    dropped_data += applied;
                    dropped_data_bytes += applied * std::min<uint32_t>(20, source.size - rules[i].sequence * 20);
                }
                if (rules[i].type == PacketType::Ack) dropped_acks += applied;
            } else if (rules[i].kind == FaultKind::Corrupt) {
                m["corruptions"] += applied;
                changed_bytes += applied;
            }
        }
        const uint64_t packets = m["expected_packets"];
        r.check(sender.terminal() && steps < 300000, "bounded simulated-time completion");
        r.check(sink.saw_partial && sink.prepares == 1 && sink.closes == 1 && sink.audited,
                "partial/create/close/read-back lifecycle");
        r.check(m["partial_remaining"] == 0 && !receiver.cleanupFailed(), "partial cleanup");
        r.check(m["early_final_files"] == 0 && sink.invalid_publishes == 0, "publication only after verification");
        r.check(m["acked_bytes"] == source.size && m["accepted_bytes"] == source.size &&
                sink.bytes == source.size && sink.audit.bytes == source.size, "exact byte counts");
        r.check(m["acked_packets"] == packets && m["accepted_packets"] == packets &&
                m["accepted_events"] == packets && sink.writes == packets, "exact packet/event/write counts");
        r.check(m["sequence_errors"] == 0 && sender.currentSequence() == packets &&
                receiver.expectedSequence() == packets, "sequence continuity and final boundary");
        if (packets) r.check(m["last_tx_sequence"] == packets - 1 && m["last_accepted_sequence"] == packets - 1 &&
                            m["last_payload_bytes"] == source.size - (packets - 1) * 20, "last DATA frame");
        r.check(m["tx_data_packets"] - dropped_data == m["rx_data_frames"] &&
                m["tx_data_bytes"] - dropped_data_bytes == m["rx_data_bytes"] &&
                m["rx_data_frames"] == packets + dropped_acks && m["duplicate_data"] == dropped_acks,
                "transmitted/delivered/duplicate DATA accounting");
        r.check(m["retries"] == m["drops"] && m["tx_frames"] == packets + 2 + m["retries"],
                "actual retry/drop/frame accounting");
        r.check(source.offset == source.size && source.bytes_read == uint64_t(source.size) * 2 && source.resets == 3,
                "stream inspection and send passes");
        r.check(status.bytes_transferred == source.size && status.total_packets == packets &&
                status.retry_count == sender.totalRetries() && status.error == sender.error(), "service status counters");
        r.check(sink.audit.crc == receiver.calculatedCrc32(), "persisted/receiver CRC agreement");
        if (corrupt) {
            r.check(sender.state() == SenderState::Failed && receiver.state() == ReceiverState::Failed &&
                    status.state == FileTransfer::State::Failed && !sender.peerComplete(), "no false success");
            r.check(sender.error() == ErrorCode::CrcMismatch && receiver.error() == ErrorCode::CrcMismatch,
                    "CRC mismatch propagated to both endpoints (code 16)");
            r.check(sink.audit.crc != sender.crc32() && sink.audit.errors == changed_bytes && changed_bytes > 0,
                    "persisted corruption measured and detected");
            r.check(sink.publish_calls == 0 && sink.publishes == 0 && m["final_exists"] == 0 && sink.removes == 1,
                    "corruption rejected before publish; partial removed");
        } else {
            r.check(sender.state() == SenderState::Completed && receiver.state() == ReceiverState::Completed &&
                    status.state == FileTransfer::State::Completed && sender.peerComplete(), "successful terminal states");
            r.check(sender.error() == ErrorCode::None && receiver.error() == ErrorCode::None, "no terminal errors");
            r.check(sink.audit.errors == 0 && sink.audit.crc == sender.crc32(), "end-to-end CRC and byte equality");
            r.check(sink.publish_calls == 1 && sink.publishes == 1 && m["final_exists"] == 1 && sink.removes == 0,
                    "exactly one final publication");
            if (m["final_exists"]) {
                const Audit final = auditFile(sink.final, source);
                r.check(final.bytes == source.size && final.errors == 0 && final.crc == sender.crc32(),
                        "published output read-back");
            }
        }
        if (stale_probe && have_previous) r.check(m["stale_frames_ignored"] == 2, "stale transfer ID rejection");
        if (saw_data) {
            previous_data = latest_data;
            previous_ack = latest_ack;
            have_previous = true;
        }
        r.check(transport.queuedFrames() == 0, "transport fully drained");
        return r;
    }
};

FaultRule drop(PacketType type, uint32_t sequence = 0, uint32_t repeat = 1)
{
    FaultRule r;
    r.type = type;
    r.destination = (type == PacketType::Ready || type == PacketType::Ack || type == PacketType::Complete)
                        ? Destination::Sender : Destination::Receiver;
    r.sequence = sequence;
    r.any_sequence = type != PacketType::Data && type != PacketType::Ack;
    r.repeat_count = repeat;
    return r;
}

FaultRule corruption(uint32_t byte, uint8_t mask)
{
    FaultRule r = drop(PacketType::Data, byte / 20);
    r.kind = FaultKind::Corrupt;
    r.corrupt_offset = kDataHeaderBytes + byte % 20;
    r.corrupt_mask = mask;
    return r;
}

int campaign(const fs::path& fixture, const fs::path& output)
{
    fs::create_directories(output);
    std::ofstream records(output / "transfers.jsonl");
    require(bool(records), "cannot open measurement file");
    bool passed = true;
    auto save = [&](Record r) {
        r.emit(records);
        if (!r.failures.empty()) {
            passed = false;
            for (const auto& failure : r.failures) std::cerr << r.category << '/' << r.name << ": " << failure << '\n';
        }
    };
    {
        Harness h(output / "large");
        Source source(kSongBytes, kSeed, 7, fixture);
        save(h.run(source, "large", "song.u8"));
    }
    {
        Harness h(output / "targeted");
        const std::array<PacketType, 6> types = {PacketType::Start, PacketType::Ready, PacketType::Data,
                                               PacketType::Ack, PacketType::End, PacketType::Complete};
        const std::array<const char*, 6> names = {"START", "READY", "DATA", "ACK", "END", "COMPLETE"};
        for (size_t i = 0; i < types.size(); ++i) {
            Source source(4097, kSeed + uint32_t(i), 7);
            save(h.run(source, "targeted", names[i], {drop(types[i])}));
        }
    }
    {
        Harness h(output / "reliability");
        for (uint32_t i = 0; i < 100; ++i) {
            const uint32_t seed = kSeed + i;
            uint32_t random = seed;
            const uint32_t size = 4096 + nextRandom(random) % 61441;
            Source source(size, seed, 1 + nextRandom(random) % 31);
            std::vector<FaultRule> rules;
            for (PacketType type : {PacketType::Start, PacketType::Ready, PacketType::End, PacketType::Complete})
                rules.push_back(drop(type, 0, 1 + nextRandom(random) % 2));
            const uint32_t packets = (size + 19) / 20;
            for (uint32_t j = 0; j < 8; ++j) {
                const uint32_t begin = packets * j / 8, end = packets * (j + 1) / 8;
                const uint32_t seq = begin + nextRandom(random) % (end - begin);
                rules.push_back(drop(PacketType::Data, seq, 1 + nextRandom(random) % 2));
                rules.push_back(drop(PacketType::Ack, seq, 1 + nextRandom(random) % 2));
            }
            save(h.run(source, "reliability", std::to_string(i), rules));
        }
    }
    {
        Harness h(output / "corruption");
        uint32_t index = 0;
        for (uint32_t position : {0u, 19u, 20u, 2048u, 4079u, 4096u, 1310719u}) {
            for (uint8_t mask : {0x01, 0x80, 0x55, 0xFF}) {
                const uint32_t size = position == 1310719u ? 1310720 : 4097;
                Source source(size, kSeed + index, 7);
                save(h.run(source, "corruption", std::to_string(index), {corruption(position, mask)}, true));
                Source recovery(21, kSeed ^ (++index), 3);
                save(h.run(recovery, "corruption_recovery", std::to_string(index), {}, false, true));
            }
        }
    }
    {
        Harness h(output / "maximum");
        Source source(1310720, kSeed, 13);
        save(h.run(source, "maximum", "65536-packets"));
    }
    {
        Harness h(output / "stress");
        constexpr std::array<uint32_t, 10> sizes = {0, 1, 19, 20, 21, 255, 4097, 65535, 65536, 1310720};
        std::vector<Record> results;
        // Deliberately do not reset/reconstruct the service, receiver, or sink.
        for (uint32_t i = 0; i < 100; ++i) {
            Source source(sizes[i % sizes.size()], kSeed + 0x10000u + i, 1 + i % 31);
            results.push_back(h.run(source, "stress", std::to_string(i), {}, false, true));
        }
        // Re-read ALL prior publications after the last transfer, detecting
        // cross-transfer overwrites as well as corruption of the newest file.
        for (auto& r : results) {
            Source source(uint32_t(r.n["expected_bytes"]), uint32_t(r.n["seed"]), 4096);
            const Audit audit = auditFile(output / r.output, source);
            r.n["preserved_output_errors"] = audit.errors;
            r.check(audit.errors == 0 && audit.bytes == source.size && audit.crc == r.n["source_crc32"],
                    "earlier publication preserved after stress");
            save(r);
        }
    }
    return passed ? 0 : 1;
}
}  // namespace

int main(int argc, char** argv)
{
    try {
        require(argc == 3, "usage: qualification <song.u8> <new-output-directory>");
        return campaign(fs::absolute(argv[1]), fs::absolute(argv[2]));
    } catch (const std::exception& error) {
        std::cerr << "QUALIFICATION INFRASTRUCTURE FAILURE: " << error.what() << '\n';
        return 2;
    }
}
