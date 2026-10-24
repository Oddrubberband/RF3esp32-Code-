"""Real ReceiverSession + extracted production RX/STOP/guard methods, fake HAL.
No hardware is opened. The sink writes/renames/removes real native test files.
"""
from pathlib import Path
import argparse
import importlib.util
import os
import subprocess
import shutil


def harness(root: Path) -> str:
    spec=importlib.util.spec_from_file_location("runtime",root/"tools/tests/test_host_runtime_contract.py")
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    cpp=module.harness(root)
    main=(root/"src/main.cpp").read_text(encoding="utf-8")
    cpp=cpp[:cpp.index("int main(int argc,char** argv)")]
    cpp=cpp.replace('#include <fstream>','#include <fstream>\n#include <sstream>\n#include "host_rx_command_guard.hpp"\n#include "host_transfer_guard.hpp"')
    cpp=cpp.replace('#define ESP_LOGI(...) testLog(__VA_ARGS__)','#define ESP_LOGI(...) testLog(__VA_ARGS__)\n#define ESP_LOGW(...) testLog(__VA_ARGS__)')
    cpp=cpp.replace('    unsigned refresh_count=0, send_attempts=0;','''    unsigned refresh_count=0, send_attempts=0, leave_count=0, enter_count=0, boot_count=0;
    bool boot(uint8_t channel){++boot_count;value.channel=channel;value.state=RadioState::Standby;return true;}
    bool stopCw(){value.state=RadioState::Standby;return true;}
    bool wake(){value.state=RadioState::Standby;return true;}''')
    cpp=cpp.replace('    bool enterRx(){return true;}','    bool enterRx(){++enter_count;value.state=RadioState::RxListening;return true;}')
    cpp=cpp.replace('    bool leaveRx(){return true;}','    bool leaveRx(){++leave_count;value.state=RadioState::Standby;return true;}')
    fake_start=cpp.index('struct FakeReceiver {');fake_end=cpp.index('\n};',fake_start)+4
    sink=r"""
std::vector<std::string> splitWords(const std::string& line){std::istringstream stream(line);std::vector<std::string> out;std::string word;while(stream>>word)out.push_back(word);return out;}
constexpr const char* kHostUploadPartialName=".host_upload.part";
constexpr int64_t kHostSessionTimeoutUs=60000000;
int64_t fake_time_us=0;
int64_t esp_timer_get_time(){return fake_time_us;}
struct TestSink {
    std::FILE* file=nullptr;
    std::string partial, final;
    unsigned opens=0, closes=0, publishes=0, removes=0;
    bool fail_remove=false;
    static ReliableTransferV2::SinkPrepareResult prepare(void* context,uint32_t id,uint32_t){
        auto& s=*static_cast<TestSink*>(context);s.partial=buildFilePath("rx_"+std::to_string(id)+".part");s.final=buildFilePath("rx_"+std::to_string(id)+".bin");
        s.file=std::fopen(s.partial.c_str(),"wb");++s.opens;return s.file?ReliableTransferV2::SinkPrepareResult::Ready:ReliableTransferV2::SinkPrepareResult::OpenFailed;
    }
    static size_t write(void* context,const uint8_t* data,size_t bytes){auto& s=*static_cast<TestSink*>(context);return std::fwrite(data,1,bytes,s.file);}
    static bool close(void* context){auto& s=*static_cast<TestSink*>(context);++s.closes;if(!s.file)return true;auto* f=s.file;s.file=nullptr;return std::fclose(f)==0;}
    static bool publish(void* context){auto& s=*static_cast<TestSink*>(context);++s.publishes;std::remove(s.final.c_str());return std::rename(s.partial.c_str(),s.final.c_str())==0;}
    static bool remove(void* context){auto& s=*static_cast<TestSink*>(context);++s.removes;if(s.fail_remove)return false;return std::remove(s.partial.c_str())==0||errno==ENOENT;}
    static ReliableTransferV2::SinkCallbacks callbacks(){return {&prepare,&write,&close,&publish,&remove};}
};
"""
    cpp=cpp[:fake_start]+sink+cpp[fake_end:]
    cpp=cpp.replace('    FakeReceiver receiver_;','''    TestSink sink_;
    ReliableTransferV2::ReceiverSession receiver_{&sink_,TestSink::callbacks()};
    FileTransfer::Service transfer_service_;
    struct TextAssembler {void reset(){}} text_message_assembler_;
    bool last_carrier_detected_=false;
    uint32_t bluetooth_dispatch_epoch_=1;
    std::atomic_bool bluetooth_client_connected_{true};
    std::atomic<uint32_t> bluetooth_epoch_{1};
    unsigned stop_loop_calls_=0, side_effects_=0;
    bool stopLoopAndWait(){++stop_loop_calls_;loop_config_={};loop_owns_radio_=false;return true;}
    bool isRemoteCommandAllowed(const std::vector<std::string>&){return true;}''')
    cpp=cpp.replace('    struct HostSession {bool active=false;} host_upload_, host_download_;','    struct HostSession {bool active=false;std::FILE* file=nullptr;CommandOrigin owner=CommandOrigin::Local;uint32_t epoch=0;int64_t last_activity_us=0;} host_upload_, host_download_;\n    using HostUploadStorage=HostSession;\n    using HostDownloadStorage=HostSession;')
    cpp=cpp.replace('struct Incoming {std::FILE* file=nullptr;}', 'struct Incoming {std::FILE* file=nullptr;std::string partial_name;}')
    cpp=cpp.replace('bool takeRadio(TickType_t){','bool takeRadio(TickType_t=100){')
    old='    bool ensureStandbyLocked(){assert(radio_held);if(manager_.value.state==RadioState::Fault)return false;manager_.value.state=RadioState::Standby;return true;}\n'
    cpp=cpp.replace(old,'')
    methods=''
    methods+=main[main.index('    void discardIncomingFileTransfer('):main.index('    bool isLoopActive()')]
    methods+=main[main.index('    bool ensureStandbyLocked()'):main.index('    bool isRemoteCommandAllowed(')]
    methods+=main[main.index('    bool commandStop()'):main.index('    bool commandStandby()')]
    methods+=main[main.index('    bool hasActiveIncomingReceiveLocked()'):main.index('    bool handleCommand(')]
    methods+=main[main.index('    void resetHostUpload('):main.index('    bool commandUpload(')]
    methods+=main[main.index('    void clearLoopLocked()'):main.index('    static ReliableTransferV2::SinkPrepareResult')]
    # Exact production command-boundary prefix; a side-effect counter replaces
    # the remaining handlers, proving the active-receiver rejection comes first.
    prefix=main[main.index('    bool handleCommand('):main.index('        if (command == "ECHO") {',main.index('    bool handleCommand('))]
    prefix=prefix.replace('bool handleCommand(const std::string& line, CommandOrigin origin)','bool guardedAttempt(const std::string& line, CommandOrigin origin, bool result=true)')
    methods+=prefix+'        ++side_effects_;\n        return result;\n    }\n'
    cpp=cpp.rstrip()[:-2]+methods.replace('RadioManager','FakeRadioManager')+'\n};\n'
    tests=r"""
ReliableTransferV2::ReceiverEvent feed(DemoConsoleApp& app,ProtocolV2::Packet packet,uint64_t time){
    ProtocolV2::Frame frame{},reply{};assert(ProtocolV2::encode(packet,frame));return app.receiver_.onFrame(frame.data(),frame.size(),time,reply);
}
std::vector<uint8_t> payload(){std::vector<uint8_t> out(43);for(size_t i=0;i<out.size();++i)out[i]=static_cast<uint8_t>(i*37);return out;}
void beginPartial(DemoConsoleApp& app,uint32_t id){
    auto bytes=payload();ProtocolV2::Packet start{};start.type=ProtocolV2::PacketType::Start;start.transfer_id=id;start.total_size=bytes.size();start.total_packets=ProtocolV2::packetCountForSize(bytes.size());start.crc32=ProtocolV2::calculateCrc32(bytes.data(),bytes.size());
    assert(feed(app,start,1)==ReliableTransferV2::ReceiverEvent::ResponseReady);
    ProtocolV2::Packet data{};data.type=ProtocolV2::PacketType::Data;data.transfer_id=id;data.sequence=0;data.payload_length=20;std::copy_n(bytes.begin(),20,data.payload.begin());
    assert(feed(app,data,2)==ReliableTransferV2::ReceiverEvent::DataAccepted);
    app.active_api_receive_id_=id;app.decoded_rx_packet_count_=1;app.saved_rx_file_count_=7;app.carrier_event_count_=9;
}
void finish(DemoConsoleApp& app,uint32_t id){
    auto bytes=payload();for(uint16_t sequence=1;sequence<3;++sequence){ProtocolV2::Packet data{};data.type=ProtocolV2::PacketType::Data;data.transfer_id=id;data.sequence=sequence;const size_t offset=sequence*20;data.payload_length=static_cast<uint8_t>(std::min<size_t>(20,bytes.size()-offset));std::copy_n(bytes.begin()+offset,data.payload_length,data.payload.begin());assert(feed(app,data,3+sequence)==ReliableTransferV2::ReceiverEvent::DataAccepted);}
    ProtocolV2::Packet end{};end.type=ProtocolV2::PacketType::End;end.transfer_id=id;end.total_size=bytes.size();end.total_packets=3;end.crc32=ProtocolV2::calculateCrc32(bytes.data(),bytes.size());
    assert(feed(app,end,9)==ReliableTransferV2::ReceiverEvent::Completed);app.active_api_receive_id_=0;
    std::ifstream file(app.sink_.final,std::ios::binary);std::vector<uint8_t> actual((std::istreambuf_iterator<char>(file)),{});assert(actual==bytes);assert(app.receiver_.calculatedCrc32()==end.crc32);assert(app.sink_.publishes==1);
}
int main(int argc,char** argv){
    assert(argc==2);file_root=argv[1];std::filesystem::create_directories(file_root);
    DemoConsoleApp app;app.manager_.value.state=RadioState::RxListening;app.manager_.value.channel=76;app.manager_.value.power_level=2;
    beginPartial(app,0x12345678);
    const auto id=app.receiver_.transferId(), accepted=app.receiver_.acceptedBytes();const auto* sink=app.sink_.file;
    assert(app.commandRx());assert(app.commandRx());
    assert(app.receiver_.transferId()==id&&app.receiver_.acceptedBytes()==accepted&&app.sink_.file==sink);
    assert(app.sink_.opens==1&&app.sink_.closes==0&&app.sink_.removes==0);
    assert(app.manager_.leave_count==0&&app.manager_.enter_count==0&&app.stop_loop_calls_==0);
    assert(app.decoded_rx_packet_count_==1&&app.saved_rx_file_count_==7&&app.carrier_event_count_==9);
    const std::vector<std::string> refused={"CHANNEL 83","POWER 3","STANDBY","SLEEP","WAKE","POWERDOWN","TESTMODE ON","TESTMODE OFF","TX file.bin","TXHEX 612e62696e","SELECT file.bin","SELECTHEX 612e62696e","TEXT hi","REMOTE STATUS","MORSE hi","CW START","CW LOOP 10 10","STORAGE DELETE 612e62696e","UPLOAD BEGIN x 1 00000000","UPLOAD DATA 0 00","DOWNLOAD BEGIN x","TESTTX 43"};
    for(const auto& line:refused){assert(!app.guardedAttempt(line,CommandOrigin::Bluetooth));assert(app.side_effects_==0);assert(!app.receive_mutation_reserved_);assert(app.receiver_.transferId()==id&&app.receiver_.acceptedBytes()==accepted&&app.sink_.file==sink);}
    for(const auto& line:{"STATUS","HELLO","FILES","HELP","STORAGE LIST 0","RX","STOP","CW STOP","UPLOAD CANCEL","DOWNLOAD CANCEL"})assert(app.guardedAttempt(line,CommandOrigin::Local));
    assert(app.receiver_.active());app.printStatus();std::cout<<app.buildStatusJson()<<'\n';
    finish(app,id);assert(app.receiver_.state()==ReliableTransferV2::ReceiverState::Completed);
    assert(app.commandRx());assert(app.receiver_.state()==ReliableTransferV2::ReceiverState::Completed);
    app.printStatus();std::cout<<app.buildStatusJson()<<'\n';
    assert(app.receiver_.reset());beginPartial(app,0x87654321);const auto cancel_bytes=app.receiver_.acceptedBytes();
    assert(app.commandStop());assert(app.receiver_.state()==ReliableTransferV2::ReceiverState::Cancelled);
    assert(app.receiver_.transferId()==0x87654321&&app.receiver_.acceptedBytes()==cancel_bytes&&app.receiver_.error()==ProtocolV2::ErrorCode::Cancelled);
    assert(!std::filesystem::exists(app.sink_.partial)&&app.sink_.file==nullptr);assert(app.manager_.value.state==RadioState::Standby);
    app.printStatus();std::cout<<app.buildStatusJson()<<'\n';
    assert(app.receiver_.reset());app.active_api_receive_id_=0;
    {HostRxCommandGuard::Reservation scope;assert(app.beginReceiveMutation({"CHANNEL","80"},CommandOrigin::Local));scope.acquired(app.receive_mutation_reserved_);assert(app.receiveWorkerPaused());}
    assert(!app.receiveWorkerPaused());
    assert(!app.guardedAttempt("CHANNEL bad",CommandOrigin::Local,false));assert(!app.receiveWorkerPaused());
    app.host_upload_.active=true;app.host_session_owns_rx_=true;assert(app.receiveWorkerPaused());app.resetHostUpload(false);assert(!app.receiveWorkerPaused());
    app.host_upload_.active=true;app.host_download_.active=true;app.host_session_owns_rx_=true;app.resetHostUpload(false);assert(app.receiveWorkerPaused());app.resetHostDownload();assert(!app.receiveWorkerPaused());
    app.loop_owns_radio_=true;app.loop_config_.active=true;assert(app.receiveWorkerPaused());app.clearLoopLocked();assert(!app.receiveWorkerPaused());
    // Actual expiry/reset helpers are called by the 100ms host-maintenance task.
    fake_time_us=60000001;app.host_upload_.active=true;app.host_session_owns_rx_=true;app.expireHostSessions();assert(!app.host_upload_.active&&!app.receiveWorkerPaused());
    fake_time_us=0;app.host_download_.active=true;app.host_download_.owner=CommandOrigin::Bluetooth;app.host_download_.epoch=1;app.host_session_owns_rx_=true;app.bluetooth_client_connected_=false;
    app.expireHostSessions();assert(!app.host_download_.active&&!app.receiveWorkerPaused());
    app.bluetooth_client_connected_=true;app.bluetooth_epoch_=2;app.host_upload_.active=true;app.host_upload_.owner=CommandOrigin::Bluetooth;app.host_upload_.epoch=1;app.host_session_owns_rx_=true;
    app.expireHostSessions();assert(!app.host_upload_.active&&!app.receiveWorkerPaused());
    app.host_download_.active=true;app.host_download_.owner=CommandOrigin::Bluetooth;app.host_download_.epoch=2;app.host_session_owns_rx_=true;app.expireHostSessions();assert(app.host_download_.active&&app.receiveWorkerPaused());
    app.resetHostDownload();assert(!app.receiveWorkerPaused());
    app.radio_available=false;assert(!app.guardedAttempt("POWER 3",CommandOrigin::Local));assert(!app.receiveWorkerPaused());app.radio_available=true;
    DemoConsoleApp fault;fault.manager_.value.state=RadioState::RxListening;fault.manager_.value.power_level=2;beginPartial(fault,0xaabbccdd);fault.manager_.value.state=RadioState::Fault;
    assert(!fault.commandRx());assert(fault.receiver_.active()&&fault.sink_.file!=nullptr&&fault.manager_.boot_count==0);
    assert(fault.commandStop());assert(fault.receiver_.state()==ReliableTransferV2::ReceiverState::Cancelled&&fault.manager_.boot_count==1&&!std::filesystem::exists(fault.sink_.partial));
    DemoConsoleApp cleanup;cleanup.manager_.value.state=RadioState::RxListening;cleanup.manager_.value.power_level=2;beginPartial(cleanup,0xabcdef01);cleanup.sink_.fail_remove=true;
    assert(!cleanup.commandStop());assert(cleanup.receiver_.state()==ReliableTransferV2::ReceiverState::Cancelled&&cleanup.receiver_.error()==ProtocolV2::ErrorCode::CleanupFailed);
    assert(std::filesystem::exists(cleanup.sink_.partial));cleanup.sink_.fail_remove=false;assert(cleanup.receiver_.reset());assert(!std::filesystem::exists(cleanup.sink_.partial));
    std::cerr<<"Real receiver repeat-RX/partial-sink/CRC completion/command guard/STOP cancellation checks passed for "<<HardwareProfile::kSelectedName<<'\n';
}
"""
    return cpp+tests


def main() -> None:
    parser=argparse.ArgumentParser();parser.add_argument("--output",type=Path,required=True);parser.add_argument("--compiler",default=shutil.which("g++") or r"C:\msys64\ucrt64\bin\g++.exe");args=parser.parse_args()
    root=Path(__file__).resolve().parents[2];out=args.output.resolve();out.mkdir(parents=True,exist_ok=True);source=out/"production_rx_guard_harness.cpp";source.write_text(harness(root),encoding="utf-8")
    env=os.environ.copy();env["PATH"]=str(Path(args.compiler).parent)+os.pathsep+env["PATH"]
    for profile,pins in ((1,(17,27,16)),(2,(27,5,26))):
        binary=out/f"rx_guard_profile_{profile}.exe";flags=["-DRF3_BLUETOOTH_CONTROL_ENABLED=1",f"-DRF3_HARDWARE_PROFILE_ID={profile}",f"-DNRF24_CE_PIN={pins[0]}",f"-DNRF24_CSN_PIN={pins[1]}",f"-DNRF24_IRQ_PIN={pins[2]}","-DNRF24_SCK_PIN=18","-DNRF24_MOSI_PIN=23","-DNRF24_MISO_PIN=19"]
        subprocess.run([args.compiler,"-std=c++17","-Wall","-Wextra","-Werror","-pedantic","-pthread","-I",str(root/"include"),*flags,str(source),"-o",str(binary)],check=True,env=env)
        run=subprocess.run([str(binary),str(out/f"files_profile_{profile}")],check=True,env=env,capture_output=True);(out/f"rx_status_profile_{profile}.txt").write_bytes(run.stdout);print(run.stderr.decode(errors="replace"),end="")
if __name__=="__main__":main()
