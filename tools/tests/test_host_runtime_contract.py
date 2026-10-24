"""Compile extracted production status/encoded-command methods with fake task/HAL adapters.

No ESP/COM/Bluetooth access occurs. A real temporary directory exercises exact
filename lookup, while runtime locks/radio/receiver are controlled test doubles.
"""
from pathlib import Path
import argparse
import os
import subprocess
import shutil


def harness(root: Path) -> str:
    main = (root / "src/main.cpp").read_text(encoding="utf-8")
    report = main[main.index("struct ProtocolTransferReport {"):main.index("const char* loopModeName")]
    capture = main[main.index("    static uint32_t queuedTransferIdEntry"):main.index("    bool dispatchHttpCommand")]
    command = main[main.index("    bool commandStoredName("):main.index("    bool commandSelect(")]
    human = main[main.index("    void printStatus()"):main.index("    static uint32_t queuedTransferIdEntry")]
    fragments = (report + capture + command + human).replace("RadioManager", "FakeRadioManager")
    before = r"""
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>
#include <sys/stat.h>
#include "radio_manager.hpp"
#include "file_transfer_service.hpp"
#include "transfer_rate_limiter.hpp"
#include "reliable_transfer_v2.hpp"
#include "host_storage_protocol.hpp"
#include "host_status_protocol.hpp"
#include "host_transfer_protocol.hpp"
#include "host_upload_publish.hpp"
#include "hardware_profile.hpp"
using TickType_t = unsigned;
using SemaphoreHandle_t = int;
using portMUX_TYPE = std::mutex;
#define portMUX_INITIALIZER_UNLOCKED {}
#define portENTER_CRITICAL(m) (m)->lock()
#define portEXIT_CRITICAL(m) (m)->unlock()
constexpr int pdTRUE=1;
constexpr TickType_t pdMS_TO_TICKS(unsigned value){return value;}
int xSemaphoreTake(SemaphoreHandle_t, TickType_t){return pdTRUE;}
void xSemaphoreGive(SemaphoreHandle_t){}
namespace WifiControlConfig {
constexpr const char* kNodeName = RF3_HARDWARE_PROFILE_ID == 1 ? "rf3-pcb" : "rf3-devboard";
}
constexpr bool kWirelessControlEnabled = false;
constexpr bool kWirelessControlAutoRx = false;
constexpr bool kWifiControlEnabled = false;
constexpr bool kBluetoothControlEnabled = true;
enum class CommandOrigin { Local, Bluetooth, Remote, Http };
enum class LoopMode { None, Tx, Cw, Morse };
struct FileInfo {std::string name; size_t bytes=0;};
struct LoopConfig {
    LoopMode mode=LoopMode::None;
    bool active=false, infinite=false, restore_rx_after_completion=false;
    uint32_t remaining_iterations=0, completed_iterations=0;
    std::string file_name, morse_text;
    uint32_t cw_on_ms=0, cw_off_ms=0, cw_report_every=0;
    uint8_t channel=76, power_level=3;
};
const char* loopModeName(LoopMode mode){
    switch(mode){case LoopMode::Tx:return "TxLoop";case LoopMode::Cw:return "CwLoop";
        case LoopMode::Morse:return "Morse";default:return "None";}
}
class FakeRadioManager {
public:
    RadioStatus value{};
    unsigned refresh_count=0, send_attempts=0;
    bool sendPayload(const uint8_t*,size_t){++send_attempts;value.state=RadioState::Fault;value.last_fault=3;return false;}
    bool enterRx(){return true;}
    bool receivePayload(uint8_t*,size_t,size_t&){return false;}
    bool leaveRx(){return true;}
    RadioStatus status() const{return value;}
    void refreshSnapshot(){++refresh_count;}
    bool hasPendingRx(){return false;}
    static const char* stateName(RadioState s){
        switch(s){case RadioState::Standby:return "Standby";case RadioState::RxListening:return "RxListening";
        case RadioState::TxBusy:return "TxBusy";case RadioState::Fault:return "Fault";
        case RadioState::Sleep:return "Sleep";case RadioState::PowerDown:return "PowerDown";
        case RadioState::CwTest:return "CwTest";default:return "Boot";}
    }
};
std::string file_root;
std::string buildFilePath(const std::string& name){return file_root+"/"+name;}
bool statFileSize(const std::string& path, size_t& bytes){
    struct stat info{};
    if(::stat(path.c_str(),&info)!=0 || info.st_size<0)return false;
    bytes=static_cast<size_t>(info.st_size);return true;
}
std::string uppercaseCopy(std::string value){
    for(auto& c:value)c=static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return value;
}
uint32_t nextTransferId(){static std::atomic<uint32_t> id{0x100};return ++id;}
constexpr const char* TAG="native";
constexpr uint32_t kTransferRateLimitBytesPerSecond=0;
template<class... T> void testLog(T&&...){}
#define ESP_LOGE(...) testLog(__VA_ARGS__)
#define ESP_LOGI(...) testLog(__VA_ARGS__)
uint64_t monotonicMilliseconds(){static uint64_t clock=0;return ++clock;}
void delayAtLeastMs(unsigned){}
using HttpStatusSnapshot=HostStatusProtocol::Snapshot;
struct FakeReceiver {
    bool active_value=false;
    bool active()const{return active_value;}
    ReliableTransferV2::ReceiverState state()const{return ReliableTransferV2::ReceiverState::Idle;}
    ProtocolV2::ErrorCode error()const{return ProtocolV2::ErrorCode::None;}
    uint32_t transferId()const{return 0;}
    uint32_t acceptedBytes()const{return 0;}
    uint32_t totalSize()const{return 0;}
    uint32_t expectedSequence()const{return 0;}
    uint32_t totalPackets()const{return 0;}
    uint32_t calculatedCrc32()const{return 0;}
};
"""
    io_functions = main[main.index("bool resetFileSource("):main.index("uint32_t nextTransferId()")]
    sending = main[main.index("void updateTransferReport("):main.index("class DemoConsoleApp {")]
    # Report publication functions are copied at namespace scope. Production
    # member methods are copied verbatim inside the controlled test adapter.
    report = report.replace("RadioManager", "FakeRadioManager")
    member_fragments = (capture + command + human).replace("RadioManager", "FakeRadioManager")
    adapter = r"""
class DemoConsoleApp {
public:
    FakeRadioManager manager_;
    ProtocolTransferReport last_tx_report_{};
    SemaphoreHandle_t status_mutex_=1;
    HttpStatusSnapshot cached_host_status_{};
    char selected_status_name_[HostStorageProtocol::kFilenameMaxBytes+1]{};
    std::string selected_file_, last_morse_text_;
    bool filesystem_ready_=true, radio_available=true, loop_available=true;
    bool radio_held=false, loop_held=false;
    unsigned radio_take_count=0;
    std::atomic_bool receive_mutation_reserved_{false}, host_session_owns_rx_{false}, loop_owns_radio_{false};
    std::atomic_bool test_mode_{false}, loop_stop_requested_{false};
    std::atomic<uint32_t> test_tx_count_{0}, test_tx_bytes_{0}, text_tx_count_{0}, text_rx_count_{0};
    uint32_t decoded_rx_packet_count_=0, raw_rx_packet_count_=0, missing_rx_packet_count_=0;
    uint32_t carrier_event_count_=0, saved_rx_file_count_=0, saved_rx_byte_count_=0, active_api_receive_id_=0;
    std::string last_saved_rx_file_;
    struct HostSession {bool active=false;} host_upload_, host_download_;
    struct Incoming {std::FILE* file=nullptr;} incoming_file_;
    FakeReceiver receiver_;
    LoopConfig loop_config_;
    std::vector<std::string> replies;
    bool takeRadio(TickType_t){++radio_take_count;if(!radio_available)return false;assert(!radio_held);radio_held=true;return true;}
    void giveRadio(){assert(radio_held);radio_held=false;}
    bool takeLoop(TickType_t){assert(!radio_held);if(!loop_available)return false;assert(!loop_held);loop_held=true;return true;}
    void giveLoop(){assert(loop_held);loop_held=false;}
    LoopConfig loopSnapshot(){assert(!radio_held);return loop_config_;}
    bool ensureStandbyLocked(){assert(radio_held);if(manager_.value.state==RadioState::Fault)return false;manager_.value.state=RadioState::Standby;return true;}
    void emitHostTransferResponse(CommandOrigin,const std::string& text){replies.push_back(text);}
"""
    tests = r"""
};
int main(int argc,char** argv){
    assert(argc==2 || argc==3);
    file_root=argv[1];
    std::filesystem::create_directories(file_root);
    DemoConsoleApp app;
    app.manager_.value.state=RadioState::Standby;
    app.manager_.value.power_level=2;
    app.manager_.value.channel=83;
    const std::string name=u8"tx caf\u00e9 file.bin";
    std::ofstream(buildFilePath(name),std::ios::binary).write("abc",3);
    std::ofstream(buildFilePath("LOOP"),std::ios::binary).write("abcd",4);
    const auto hex=HostStorageProtocol::encodeName(name);
    assert(app.commandStoredName({"SELECTHEX",hex},CommandOrigin::Local));
    assert(app.selected_file_==name && app.replies.back()=="OK SELECTHEX "+hex);
    assert(copyTransferReport(app.last_tx_report_).state==ReliableTransferV2::SenderState::Idle);
    for(const auto origin:{CommandOrigin::Remote,CommandOrigin::Http}){
        assert(!app.commandStoredName({"TXHEX",hex},origin));
        assert(app.replies.back().find("host_storage_unavailable")!=std::string::npos);
    }
    for(const std::string bad:{"2e68696464656e","2e2e","2f","5c","610062","c0af","zz","0"})
        assert(!app.commandStoredName({"TXHEX",bad},CommandOrigin::Bluetooth));
    app.host_upload_.active=true;
    assert(!app.commandStoredName({"TXHEX",hex},CommandOrigin::Bluetooth));
    app.host_upload_.active=false;
    app.receiver_.active_value=true;
    assert(!app.commandStoredName({"TXHEX",hex},CommandOrigin::Bluetooth));
    app.receiver_.active_value=false;
    std::ofstream(buildFilePath(HostUploadPublish::kJournal)).write("j",1);
    assert(!app.commandStoredName({"SELECTHEX",hex},CommandOrigin::Local));
    std::filesystem::remove(buildFilePath(HostUploadPublish::kJournal));
    app.test_mode_=true;
    assert(!app.commandStoredName({"TXHEX",hex},CommandOrigin::Local));
    assert(app.commandStoredName({"SELECTHEX",hex},CommandOrigin::Local));
    app.test_mode_=false;
    app.manager_.value.state=RadioState::Fault;
    assert(!app.commandStoredName({"TXHEX",hex},CommandOrigin::Local));
    app.manager_.value.state=RadioState::Standby;
    assert(app.commandStoredName({"TXHEX",hex},CommandOrigin::Bluetooth));
    auto queued=copyTransferReport(app.last_tx_report_);
    assert(queued.transfer_id!=0 && queued.state==ReliableTransferV2::SenderState::Preparing);
    assert(queued.total_bytes==3 && queued.file_name==name);
    assert(queued.radio.channel==83 && queued.radio.power_level==2);
    assert(app.queuedTransferIdEntry(&app)==queued.transfer_id);
    FileTransfer::Service real_service(&DemoConsoleApp::queuedTransferIdEntry,&app);
    std::atomic_bool not_stopped{false};
    // Actual sender entry executes missing-source failure without touching HAL.
    auto missing=queued;
    assert(!sendDataFile(app.manager_,real_service,buildFilePath("missing.bin").c_str(),&not_stopped,&missing));
    assert(missing.transfer_id==queued.transfer_id && missing.state==ReliableTransferV2::SenderState::Failed);
    assert(missing.error==ProtocolV2::ErrorCode::SourceRead && app.manager_.send_attempts==0);
    // Actual source preparation + Service ID callback retain accepted identity;
    // fake transport failure drives actual retry exhaustion and final publication.
    publishTransferReport(app.last_tx_report_,queued);
    assert(!sendDataFile(app.manager_,real_service,buildFilePath(name).c_str(),&not_stopped,&app.last_tx_report_));
    const auto transport_failed=copyTransferReport(app.last_tx_report_);
    assert(transport_failed.transfer_id==queued.transfer_id);
    assert(transport_failed.state==ReliableTransferV2::SenderState::Failed && !transport_failed.peer_complete);
    assert(app.manager_.send_attempts>0);
    app.manager_.value.state=RadioState::Standby;
    app.manager_.value.last_fault=0;
    publishTransferReport(app.last_tx_report_,queued);
    // The actual source-inspection error path retains the accepted request ID.
    const std::string large_name="too-large.bin";
    {std::ofstream large(buildFilePath(large_name),std::ios::binary);large.seekp(ProtocolV2::kMaxFileSize);large.put('x');}
    ProtocolTransferReport large_report;
    prepareTransferReport(large_report,FileInfo{large_name,ProtocolV2::kMaxFileSize+1},app.manager_.status(),nextTransferId());
    const auto large_id=large_report.transfer_id;
    const auto prior_attempts=app.manager_.send_attempts;
    assert(!sendDataFile(app.manager_,real_service,buildFilePath(large_name).c_str(),&not_stopped,&large_report));
    assert(large_report.transfer_id==large_id && large_report.state==ReliableTransferV2::SenderState::Failed);
    assert(large_report.error==ProtocolV2::ErrorCode::UnsupportedSize && app.manager_.send_attempts==prior_attempts);
    assert(app.replies.back()=="OK TXHEX "+hex);
    assert(app.loop_config_.active && app.loop_config_.file_name==name && !app.loop_config_.infinite);
    // Last-known RX metadata survives a radio-owned status capture, while live
    // RF transfer bytes/ID advance independently and preserve actual power.
    app.saved_rx_file_count_=9;app.last_saved_rx_file_=name;
    HttpStatusSnapshot first;
    assert(app.captureHttpStatus(first));
    app.radio_available=false;
    auto progress=queued;
    progress.state=ReliableTransferV2::SenderState::WaitingForDataAck;
    progress.bytes_transferred=2;
    publishTransferReport(app.last_tx_report_,progress);
    const auto refresh_before=app.manager_.refresh_count;
    HttpStatusSnapshot live;
    assert(app.captureHttpStatus(live));
    assert(app.manager_.refresh_count==refresh_before);
    assert(std::string(live.state_name)=="TxBusy" && live.tx.id==queued.transfer_id && live.tx.bytes==2);
    assert(live.channel==83 && live.power_level==2 && live.rx_saved==9 && live.rx_last_name==name);
    assert(std::string(live.loop_name)=="TxLoop");
    if(argc==2){app.printStatus();std::cout << app.buildStatusJson() << '\n';}
    // Radio/source preparation fails before transport start: same accepted ID,
    // name, and zero peer_complete survive the terminal publication.
    progress.state=ReliableTransferV2::SenderState::Failed;
    progress.error=ProtocolV2::ErrorCode::SourceRead;
    publishTransferReport(app.last_tx_report_,progress);
    app.radio_available=true;app.loop_config_={};
    HttpStatusSnapshot failed;
    assert(app.captureHttpStatus(failed));
    assert(failed.tx.id==queued.transfer_id && failed.tx.file_name==name);
    assert(std::string(failed.tx.state_name)=="Failed" && !failed.tx.peer_complete);
    // A reserved plain command word is an exact filename under TXHEX.
    assert(app.commandStoredName({"TXHEX",HostStorageProtocol::encodeName("LOOP")},CommandOrigin::Local));
    const auto reserved=copyTransferReport(app.last_tx_report_);
    assert(reserved.transfer_id!=queued.transfer_id && std::string(reserved.file_name)=="LOOP");
    assert(app.loop_config_.remaining_iterations==1 && !app.loop_config_.infinite);
    app.loop_config_={};
    app.loop_config_.active=true;app.loop_config_.mode=LoopMode::Cw;
    app.manager_.value.state=RadioState::Standby;
    HttpStatusSnapshot off;
    assert(app.captureHttpStatus(off));
    assert(std::string(off.state_name)=="Standby" && std::string(off.loop_name)=="CwLoop");
    if(argc==3){app.printStatus();std::cout << app.buildStatusJson() << '\n';}
    app.loop_available=false;
    assert(app.captureHttpStatus(off));assert(std::string(off.loop_name)=="Busy");
    app.loop_available=true;
    // Copy/update use the production nonallocating publication primitive.
    ProtocolTransferReport shared;
    std::atomic_bool done{false};
    std::thread writer([&]{for(uint32_t i=1;i<50000;++i){ProtocolTransferReport next;next.transfer_id=i;next.bytes_transferred=i^0x5a5a5a5a;next.current_sequence=i+13;publishTransferReport(shared,next);}done=true;});
    while(!done){const auto snap=copyTransferReport(shared);if(snap.transfer_id){assert(snap.bytes_transferred==(snap.transfer_id^0x5a5a5a5a));assert(snap.current_sequence==snap.transfer_id+13);}}
    writer.join();
    // Emit production USB human + rich JSON consecutive lines for GUI tests.
    app.loop_config_={};app.setSelectedFile(name);
    progress=queued;progress.state=ReliableTransferV2::SenderState::Completed;
    progress.bytes_transferred=progress.total_bytes;progress.peer_complete=true;
    publishTransferReport(app.last_tx_report_,progress);
    if(argc==2){app.printStatus();std::cout << app.buildStatusJson() << '\n';}
    std::cerr << "Production runtime status/encoded names/request IDs/guards/publication checks passed for " << HardwareProfile::kSelectedName << '\n';
}
"""
    return before + report + io_functions + sending.replace("RadioManager", "FakeRadioManager") + adapter + member_fragments + tests


def main() -> None:
    parser=argparse.ArgumentParser()
    parser.add_argument("--output",type=Path,required=True)
    parser.add_argument("--compiler",default=shutil.which("g++") or r"C:\msys64\ucrt64\bin\g++.exe")
    args=parser.parse_args()
    root=Path(__file__).resolve().parents[2]
    output=args.output.resolve();output.mkdir(parents=True,exist_ok=True)
    source=output/"production_runtime_harness.cpp"
    source.write_text(harness(root),encoding="utf-8")
    env=os.environ.copy();env["PATH"]=str(Path(args.compiler).parent)+os.pathsep+env["PATH"]
    for profile,pins in ((1,(17,27,16)),(2,(27,5,26))):
        binary=output/f"runtime_profile_{profile}.exe"
        flags=[f"-DRF3_HARDWARE_PROFILE_ID={profile}",f"-DNRF24_CE_PIN={pins[0]}",f"-DNRF24_CSN_PIN={pins[1]}",f"-DNRF24_IRQ_PIN={pins[2]}","-DNRF24_SCK_PIN=18","-DNRF24_MOSI_PIN=23","-DNRF24_MISO_PIN=19"]
        subprocess.run([args.compiler,"-std=c++17","-Wall","-Wextra","-Werror","-pedantic","-pthread","-I",str(root/"include"),*flags,str(source),"-o",str(binary)],check=True,env=env)
        run=subprocess.run([str(binary),str(output/f"files_profile_{profile}")],check=True,env=env,capture_output=True)
        (output/f"usb_status_profile_{profile}.txt").write_bytes(run.stdout)
        print(run.stderr.decode(errors="replace"),end="")
        cw=subprocess.run([str(binary),str(output/f"files_profile_{profile}"),"cw"],check=True,env=env,capture_output=True)
        (output/f"cw_off_status_profile_{profile}.txt").write_bytes(cw.stdout)

if __name__=="__main__":main()
