#include <array>
#include <cassert>
#include <iostream>
#include <sstream>
#include <vector>

#include "host_transfer_protocol.hpp"
#include "protocol_v2.hpp"

int main()
{
    using namespace HostTransferProtocol;
    static_assert(ProtocolV2::kFrameSize == 32, "Host chunk negotiation must not change RF framing");
    size_t chunk = 256;
    assert(negotiateChunk(false, "", chunk) && chunk == 64);
    assert(negotiateChunk(true, "64", chunk) && chunk == 64);
    assert(negotiateChunk(true, "256", chunk) && chunk == 256);
    assert(negotiateChunk(true, "128", chunk) && chunk == 128);
    assert(negotiateChunk(true, "065", chunk) && chunk == 65);
    for (const std::string request : {"", "0", "1", "63", "257", "99999999999999999999999999", "-1", "+64", "0x40", "256x", "64 ", " 64"}) {
        assert(!negotiateChunk(true, request, chunk));
    }
    assert(validChunkLength(64, 64));
    assert(!validChunkLength(65, 64));
    assert(validChunkLength(256, 256));
    assert(validChunkLength(1, 256));
    assert(!validChunkLength(257, 256));
    assert(!validChunkLength(0, 256));
    assert(!validChunkLength(32, 32));
    assert(downloadChunkSize(64, 1000) == 64);
    assert(downloadChunkSize(256, 1000) == 256);
    assert(downloadChunkSize(256, 17) == 17);
    assert(downloadChunkSize(256, 0) == 0);
    assert(downloadChunkSize(0, 17) == 0);
    assert(downloadChunkSize(257, 17) == 0);
    for (const std::string prefix : {"UPLOAD READY file.bin 513 12345678", "DOWNLOAD READY file.bin 513 12345678", "DOWNLOAD READYHEX 66696c652e62696e 513 12345678"}) {
        auto legacy = prefix;
        appendReadyChunk(legacy, false, 64);
        assert(legacy == prefix);
        auto negotiated = prefix;
        appendReadyChunk(negotiated, true, 256);
        assert(negotiated == prefix + " 256");
        std::istringstream fields(negotiated);
        size_t count = 0;
        std::string field;
        while (fields >> field) ++count;
        assert(count == 6);
    }
    std::vector<uint8_t> payload(513);
    for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<uint8_t>(i);
    ProtocolV2::Crc32 complete;
    complete.update(payload.data(), payload.size());
    for (const size_t negotiated : {size_t(64), size_t(256)}) {
        ProtocolV2::Crc32 crc;
        std::array<uint8_t, kMaxChunkBytes> previous{};
        size_t previous_length = 0;
        size_t total = 0;
        uint32_t next_sequence = 0;
        while (total < payload.size()) {
            const size_t length = downloadChunkSize(negotiated, payload.size() - total);
            assert(validChunkLength(length, negotiated));
            const uint8_t* data = payload.data() + total;
            assert(!matchesPrevious(next_sequence, next_sequence, data, length, previous.data(), previous_length));
            crc.update(data, length);
            std::copy(data, data + length, previous.begin());
            previous_length = length;
            total += length;
            ++next_sequence;
            // A lost ACK replays exactly the last chunk. The actual helper
            // recognizes it before firmware reaches fwrite/CRC advancement.
            assert(matchesPrevious(next_sequence - 1, next_sequence, data, length, previous.data(), previous_length));
            const auto before_crc = crc.value();
            if (!matchesPrevious(next_sequence - 1, next_sequence, data, length, previous.data(), previous_length)) {
                crc.update(data, length);
                total += length;
            }
            assert(crc.value() == before_crc && total <= payload.size());
            previous[0] ^= 1;
            assert(!matchesPrevious(next_sequence - 1, next_sequence, data, length, previous.data(), previous_length));
            previous[0] ^= 1;
        }
        assert(total == payload.size() && previous_length == 1);
        assert(crc.value() == complete.value());
    }
    const uint8_t reference[] = {'1','2','3','4','5','6','7','8','9'};
    ProtocolV2::Crc32 standard;
    standard.update(reference, sizeof(reference));
    assert(standard.value() == 0xcbf43926);
    assert(!matchesPrevious(0, 1, nullptr, 0, nullptr, 0));
    const std::string largest_command = "UPLOAD DATA 4294967295 " + std::string(kMaxChunkBytes * 2, 'f');
    assert(largest_command.size() + 1 < kConsoleLineBytes);
    std::cout << "Host chunk negotiation, 64/256-byte CRC, replay, tail and command-boundary checks passed\n";
}
