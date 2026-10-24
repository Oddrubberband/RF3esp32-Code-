#include <cassert>
#include <iostream>

#include "host_connection_protocol.hpp"

int main()
{
    using HostConnectionProtocol::hello;
    assert(hello("rf3-pcb", "custom-pcb", 2) ==
           "{\"rf3_protocol\":1,\"node_name\":\"rf3-pcb\",\"profile\":\"custom-pcb\",\"protocol\":2}");
    assert(hello("rf3-devboard", "esp32-devboard", 2) ==
           "{\"rf3_protocol\":1,\"node_name\":\"rf3-devboard\",\"profile\":\"esp32-devboard\",\"protocol\":2}");
    assert(hello("quote\"slash\\line\n", std::string("\0\x1f", 2), 2) ==
           "{\"rf3_protocol\":1,\"node_name\":\"quote\\\"slash\\\\line\\u000a\",\"profile\":\"\\u0000\\u001f\",\"protocol\":2}");
    assert(hello("", "", 255) ==
           "{\"rf3_protocol\":1,\"node_name\":\"\",\"profile\":\"\",\"protocol\":255}");
    std::cout << "4 host connection protocol tests passed\n";
}
