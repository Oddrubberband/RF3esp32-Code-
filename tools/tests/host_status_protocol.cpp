#include <cassert>
#include <iostream>
#include <limits>

#include "host_status_protocol.hpp"
#include "host_storage_protocol.hpp"

int main(int argc, char**)
{
    using namespace HostStatusProtocol;
    Snapshot s;
    s.node_name = "rf3-pcb";
    s.profile = "custom-pcb";
    s.state_name = "TxBusy";
    s.filesystem_ready = true;
    s.host_chunk_max = 256;
    s.selected_name = u8"caf\u00e9 file.bin";
    s.tx.file_name = s.selected_name;
    s.tx.state_name = "WaitingForReady";
    s.tx.id = 0x12345678;
    s.tx.total = 513;
    s.power_level = 2;
    auto json_line = json(s);
    assert(json_line.find("\"tx_id\":\"12345678\"") != std::string::npos);
    assert(json_line.find("\"tx_bytes\":\"0/513\"") != std::string::npos);
    assert(json_line.find("\"tx_state\":\"WaitingForReady\"") != std::string::npos);
    assert(json_line.find("\"peer_complete\":false") != std::string::npos);
    assert(json_line.find("\"test_mode_supported\":false") != std::string::npos);
    assert(json_line.find("\"host_stored_tx\":true") != std::string::npos);
    assert(json_line.find("\"power\":2") != std::string::npos);
    if (argc > 1) std::cout << json_line << '\n';
    s.tx.state_name = "Completed";
    s.tx.bytes = 513;
    s.tx.peer_complete = true;
    s.tx.crc32 = 0xcbf43926;
    json_line = json(s);
    assert(json_line.find("\"tx_bytes\":\"513/513\"") != std::string::npos);
    assert(json_line.find("\"tx_crc32\":\"CBF43926\"") != std::string::npos);
    assert(json_line.find("\"peer_complete\":true") != std::string::npos);
    if (argc > 1) std::cout << json_line << '\n';
    s.profile = "esp32-devboard";
    s.node_name = "rf3-devboard";
    s.hostname = "rf3-devboard";
    s.loop_name = "CwLoop";
    s.test_mode_supported = true;
    s.filesystem_ready = false;
    s.power_level = -1;
    json_line = json(s);
    assert(json_line.find("\"test_mode_supported\":true") != std::string::npos);
    assert(json_line.find("\"host_stored_tx\":false") != std::string::npos);
    assert(json_line.find("\"power\":-1") != std::string::npos);
    assert(json_line.find("\"loop\":\"CwLoop\"") != std::string::npos);
    if (argc > 1) std::cout << json_line << '\n';
    // Maximum legal filename escaping, all counters at UINT32_MAX, and the
    // longest actual state/error names. This must fit one SPP message + CRLF.
    s.selected_name = std::string(30, '"');
    s.tx.file_name = s.selected_name;
    s.rx_last_name = s.selected_name;
    s.rx_last_path = "/spiffs/" + s.selected_name;
    s.tx.state_name = "WaitingForComplete";
    s.rx.state_name = "WaitingForEnd";
    s.state_name = "RxListening";
    s.test_mode_supported = false;
    s.tx.peer_complete = false;
    s.channel = 125;
    s.tx.error_name = "packet-count-mismatch";
    s.rx.error_name = "packet-count-mismatch";
    s.selected_bytes = s.test_tx_count = s.test_tx_bytes = s.rx_packets = s.rx_stream =
        s.rx_raw = s.rx_missing = s.rx_saved = s.rx_saved_bytes = UINT32_MAX;
    s.tx.bytes = s.tx.total = s.tx.sequence = s.tx.packets = s.tx.retries = UINT32_MAX;
    s.rx.bytes = s.rx.total = s.rx.sequence = s.rx.packets = UINT32_MAX;
    s.last_fault = std::numeric_limits<int>::min();
    json_line = json(s);
    std::cerr << "Worst-case JSON bytes: " << json_line.size() << '\n';
    assert(json_line.find("response_too_large") == std::string::npos);
    assert(json_line.size() <= kMaxResponseBytes);
    if (argc > 1) std::cout << json_line << '\n';
    std::cerr << "Host status contract/escaping/size checks passed\n";
}
