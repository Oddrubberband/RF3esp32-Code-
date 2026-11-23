"""Compile exact production HELLO/DIAG dispatch with controlled host transports.
No radio/command lock adapter exists, proving the fast path does not need either.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--compiler', default=r'C:\msys64\ucrt64\bin\g++.exe')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    main_source = (root / 'src/main.cpp').read_text(encoding='utf-8')
    start = main_source.index('    bool dispatchCommand(')
    end = main_source.index('        if (!takeCommand(', start)
    dispatch = main_source[start:end] + '\n        return false;\n    }\n'
    source = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cctype>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>
#include "host_connection_protocol.hpp"
#include "host_rx_command_guard.hpp"
#include "runtime_diagnostics.hpp"
constexpr unsigned configTICK_RATE_HZ=1000;
namespace WifiControlConfig {constexpr const char* kNodeName="rf3-pcb";}
namespace HardwareProfile {constexpr const char* kSelectedName="custom-pcb";}
namespace ProtocolV2 {constexpr uint8_t kVersion=2;}
enum class CommandOrigin {Local, Bluetooth, Http, Remote};
std::vector<std::string> splitWords(const std::string& line){std::istringstream stream(line);std::vector<std::string> words;std::string word;while(stream>>word)words.push_back(word);return words;}
std::string uppercaseCopy(std::string line){for(char& c:line)c=static_cast<char>(std::toupper(static_cast<unsigned char>(c)));return line;}
class App {
public:
    std::atomic_bool bluetooth_client_connected_{true};
    std::atomic<uint32_t> bluetooth_epoch_{7};
    std::vector<std::string> sent;
    bool sendBluetoothText(const std::string& text,uint32_t epoch){assert(epoch==7);sent.push_back(text);return true;}
'''+dispatch+r'''
};
int main(){
    App app;
    assert(!HostRxCommandGuard::needsReservation("DIAG",""));
    assert(app.dispatchCommand("DIAG",CommandOrigin::Bluetooth,7));
    assert(app.sent.size()==2 && app.sent[0]=="OK DIAG\r\n");
    assert(app.sent[1].find("\"type\":\"diag\"")!=std::string::npos);
    assert(app.sent[1].size()<RuntimeDiagnostics::kJsonCapacity);
    const auto sent=app.sent.size();
    assert(!app.dispatchCommand("DIAG",CommandOrigin::Bluetooth,6));
    assert(!app.dispatchCommand("DIAG reset",CommandOrigin::Bluetooth,7));
    assert(!app.dispatchCommand("DIAG",CommandOrigin::Remote));
    assert(!app.dispatchCommand("DIAG",CommandOrigin::Http));
    app.bluetooth_client_connected_=false;
    assert(!app.dispatchCommand("DIAG",CommandOrigin::Bluetooth,7));
    assert(app.sent.size()==sent);
    assert(app.dispatchCommand("HELLO",CommandOrigin::Local));
    assert(app.dispatchCommand("DIAG",CommandOrigin::Local));
    RuntimeDiagnostics::Counters maximal;
    maximal.tx_ds.add(UINT32_MAX);maximal.tx_fifo_only.add(UINT32_MAX);
    maximal.tx_recoveries.add(UINT32_MAX);maximal.tx_attempt_timeouts.add(UINT32_MAX);
    maximal.tx_max_rt.add(UINT32_MAX);maximal.ack_timeouts.add(UINT32_MAX);
    maximal.rejected_responses.add(UINT32_MAX);
    for(auto* timing:{&maximal.rx_poll,&maximal.storage_write,&maximal.ack_wait}){
        timing->count.add(UINT32_MAX);timing->sum_us.add(UINT32_MAX);timing->max_us.add(UINT32_MAX);
    }
    const auto full=RuntimeDiagnostics::json(maximal,"esp32-devboard",1000);
    assert(full.size()<RuntimeDiagnostics::kJsonCapacity);
    std::printf("%s\n",full.c_str());
}
'''
    cpp = out / 'diagnostics_dispatch.cpp'
    cpp.write_text(source, encoding='utf-8')
    binary = out / 'diagnostics_dispatch.exe'
    env = os.environ.copy()
    env['PATH'] = str(Path(args.compiler).parent) + os.pathsep + env['PATH']
    subprocess.run([args.compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pedantic', '-DRF3_BLUETOOTH_CONTROL_ENABLED=1', '-I', str(root / 'include'), str(cpp), '-o', str(binary)], check=True, env=env)
    run = subprocess.run([str(binary)], capture_output=True, check=True, env=env)
    (out / 'diagnostics_dispatch.log').write_bytes(run.stdout + run.stderr)
    lines = run.stdout.decode().splitlines()
    assert len(lines)==4 and lines[1]=='OK DIAG', lines
    hello, diag, worst = map(json.loads, (lines[0],lines[2],lines[3]))
    assert hello['diagnostics'] is True and hello['build']=='RF3-USBPCB-R1-20261122'
    assert diag['type']=='diag' and diag['v']==1 and diag['tick_hz']==1000
    assert worst['tx_ds']==4294967295 and worst['ack_wait_us_max']==4294967295
    assert all(len(line)<1280 for line in lines)
    print('Production DIAG/HELLO dispatch, epoch/origin/read-only guards, JSON framing and maximal aggregate encoding PASS; maximum JSON line',len(lines[3]),'bytes')

if __name__=='__main__':main()
