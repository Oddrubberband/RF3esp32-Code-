#include <cassert>
#include <iostream>
#include <limits>

#include "host_storage_protocol.hpp"

int main(int argc, char**)
{
    using namespace HostStorageProtocol;
    assert(validFilename("README.md"));
    assert(validFilename("test file.wav"));
    assert(validFilename(u8"caf\u00e9 \u65e5\u672c.bin"));
    assert(validFilename("quote\".bin"));
    assert(validFilename(std::string(30, 'x')));
    assert(!validFilename(std::string(31, 'x')));
    for (const std::string name : {"", ".", "..", ".hidden", "one..two", "../one", "one/two", "one\\two",
                                   "host_backup.part", "HOST_REPLACE.PART", "rx_1.PaRt", "line\nfeed", "del\x7f"}) {
        assert(!validFilename(name));
    }
    assert(!validFilename(std::string("nul\0file", 8)));
    for (const std::string invalid : {"\xc0\xaf", "\xe0\x80\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xc3", "\x80"}) {
        assert(!validFilename(invalid));
    }
    std::string decoded;
    const std::string unicode_name = u8"caf\u00e9 \u65e5\u672c.bin";
    assert(decodeName(encodeName(unicode_name), decoded) && decoded == unicode_name);
    assert(decodeName("6120422e62696E", decoded) && decoded == "a B.bin");
    for (const std::string hex : {"", "a", "zz", "2e2e", "2f", "5c", "610062", "c0af", "2e68696464656e"}) {
        decoded = "stale";
        assert(!decodeName(hex, decoded) && decoded.empty());
    }
    assert(!decodeName(std::string(62, 'a'), decoded));
    assert(error("bad\"\n") == "{\"storage\":{\"op\":\"error\",\"message\":\"bad\\\"\\u000a\"}}");
    assert(deleted("quote\".bin") == "{\"storage\":{\"op\":\"delete\",\"name\":\"quote\\\".bin\"}}");
    assert(listing({}, 0, 10, 100) ==
           "{\"storage\":{\"op\":\"list\",\"offset\":0,\"next\":null,\"total\":0,\"used\":10,\"capacity\":100,\"files\":[]}}");
    std::vector<File> files{{"a", 0}, {"b", 1}, {"c", 2}, {"d", 3}, {unicode_name, 4}};
    const auto first = listing(files, 0, 10, 100);
    const auto last = listing(files, 4, 10, 100);
    assert(first.find("\"next\":4") != std::string::npos);
    assert(first.find(unicode_name) == std::string::npos);
    assert(last.find("\"next\":null") != std::string::npos);
    assert(last.find(unicode_name) != std::string::npos);
    assert(listing(files, 5, 10, 100).find("\"files\":[]") != std::string::npos);
    assert(listing(files, std::numeric_limits<size_t>::max(), 10, 100) == error("invalid_offset"));
    const std::vector<File> protected_files{{"host_backup.part", 1}};
    assert(listing(protected_files, 0, 0, 100) == error("unsupported_visible_filename"));
    std::vector<File> worst(4, {std::string(30, '"'), std::numeric_limits<size_t>::max()});
    const auto bound = listing(worst, 0, std::numeric_limits<size_t>::max(), std::numeric_limits<size_t>::max());
    assert(bound.size() <= kMaxResponseBytes && bound.find("\"op\":\"list\"") != std::string::npos);
    DeleteGuards idle;
    assert(deleteError(idle) == nullptr);
    for (int i = 0; i < 5; ++i) {
        DeleteGuards busy;
        if (i == 0) busy.host_transfer_active = true;
        if (i == 1) busy.loop_active = true;
        if (i == 2) busy.radio_active = true;
        if (i == 3) busy.receive_active = true;
        if (i == 4) busy.recovery_pending = true;
        assert(deleteError(busy) != nullptr);
    }
    StoredGuards selectable;
    assert(storedOperationError(selectable, false) == nullptr);
    assert(std::string(storedOperationError(selectable, true)) == "radio_not_ready");
    selectable.radio_ready = true;
    assert(storedOperationError(selectable, true) == nullptr);
    selectable.test_mode = true;
    assert(storedOperationError(selectable, false) == nullptr);
    assert(std::string(storedOperationError(selectable, true)) == "test_mode_blocks_RF_TX");
    for (int i = 0; i < 5; ++i) {
        StoredGuards busy;
        busy.radio_ready = true;
        if (i == 0) busy.host_transfer_active = true;
        if (i == 1) busy.loop_active = true;
        if (i == 2) busy.radio_active = true;
        if (i == 3) busy.receive_active = true;
        if (i == 4) busy.recovery_pending = true;
        assert(storedOperationError(busy, false) != nullptr);
        assert(storedOperationError(busy, true) != nullptr);
    }
    if (argc > 1) {
        std::cout << first << '\n' << last << '\n' << bound << '\n'
                  << deleted(unicode_name) << '\n' << error("disk\"failure\n") << '\n';
    } else {
        std::cout << "Host storage protocol validation, pagination, framing and activity-guard checks passed\n";
    }
}
