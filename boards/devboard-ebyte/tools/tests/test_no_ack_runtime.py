"""Execute actual file sender/sink/receiver paths with fake radio; no hardware."""
import argparse
import importlib.util
import os
from pathlib import Path
import subprocess


def harness(root):
    spec = importlib.util.spec_from_file_location("metadata", root / "tools/tests/test_file_type_metadata.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    cpp = module.harness(root)
    cpp = cpp[:cpp.index("int main(int argc,char** argv){")]
    main = (root / "src/main.cpp").read_text(encoding="utf-8")
    rx = main[main.index("    bool sendProtocolResponseLocked("):main.index("    static bool txBatteryCheckEntry(")]
    insertion = cpp.rfind("};", 0, cpp.index("class BytesSource"))
    cpp = cpp[:insertion] + rx + cpp[insertion:]
    cpp += r'''

void consecutiveNoAck(){
    DemoConsoleApp rx;
    rx.rx_no_ack_mode_=true;
    rx.manager_.value.state=RadioState::RxListening;
    unsigned copies=0, attempt=0;
    std::string prior_file;
    for(size_t length:{43u,63u,0u}){
        const auto bytes=content(length);
        const std::string name="next"+std::to_string(length)+".bin";
        {std::ofstream f(buildFilePath(name),std::ios::binary);if(length)f.write(reinterpret_cast<const char*>(bytes.data()),length);}
        DemoConsoleApp tx;
        tx.manager_.send_success=true;
        tx.manager_.value.state=RadioState::Standby;
        ProtocolTransferReport report;
        prepareTransferReport(report,FileInfo{name,length},tx.manager_.status(),nextTransferId());
        std::atomic_bool stop{false};
        assert(sendDataFileNoAck(tx.manager_,buildFilePath(name).c_str(),&stop,&report,
                                nullptr,nullptr,0,3,++attempt));
        assert(length==0 || report.work>0);
        for(const auto& frame:tx.manager_.sent_frames){
            runtime_test_clock_ms+=frame[4]==static_cast<uint8_t>(ProtocolV2::PacketType::Start)?250:10;
            if(length==43 && frame[4]==static_cast<uint8_t>(ProtocolV2::PacketType::Data) &&
               (frame[2]=='P'||ProtocolV2::loadLe16(frame.data()+9)!=0)) continue;
            rx.processRxPayload(frame.data(),frame.size());
        }
        assert(!rx.receiver_.active() && rx.manager_.send_attempts==0);
        if(length==43) {
            assert(!rx.receiver_.complete() && rx.saved_rx_file_count_==0);
        } else {
            assert(rx.receiver_.complete() && rx.saved_rx_file_count_==++copies);
            assert(rx.incoming_file_.final_name!=prior_file);
            assert(readFile(rx.incoming_file_.final_name)==bytes);
            prior_file=rx.incoming_file_.final_name;
        }
    }
}
int main(int argc,char** argv){
    assert(argc==2);file_root=argv[1];std::filesystem::create_directories(file_root);
    consecutiveNoAck();
    unsigned cases=0;
    for(size_t length:{0u,1u,20u,21u,43u,513u}){
        const std::string name="input"+std::to_string(length)+".PDF";
        const auto bytes=content(length);
        {std::ofstream f(buildFilePath(name),std::ios::binary);f.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());}
        DemoConsoleApp tx;tx.manager_.send_success=true;tx.manager_.value.state=RadioState::Standby;
        ProtocolTransferReport report;prepareTransferReport(report,FileInfo{name,length},tx.manager_.status(),nextTransferId());
        std::atomic_bool stop{false};
        assert(sendDataFileNoAck(tx.manager_,buildFilePath(name).c_str(),&stop,&report));
        assert(report.no_ack && !report.peer_complete && report.bytes_transferred==length && report.retry_count==0);
        assert(std::string(transferReportStateName(report))=="SentUnconfirmed");
        assert(tx.manager_.enter_count==0 && tx.manager_.ack_disable_calls==1);
        const unsigned packets=ProtocolV2::packetCountForSize(length);
        unsigned direct=0,repairs=0,starts=0,ends=0,markers=0,legacy_starts=0,legacy_data=0,legacy_ends=0;
        for(const auto& f:tx.manager_.sent_frames){
            if(f[2]=='M'){++markers;continue;}
            if(f[2]=='N'){
                const auto type=static_cast<ProtocolV2::PacketType>(f[4]);
                if(type==ProtocolV2::PacketType::Start) ++legacy_starts;
                else if(type==ProtocolV2::PacketType::End) ++legacy_ends;
                else {
                    assert(type==ProtocolV2::PacketType::Data);
                    assert(ProtocolV2::loadLe16(f.data()+9)==legacy_data);
                    ++legacy_data;
                }
                continue;
            }
            assert(f[2]=='G' || f[2]=='P');
            const auto type=static_cast<ProtocolV2::PacketType>(f[4]);
            if(f[2]=='P') ++repairs;
            else if(type==ProtocolV2::PacketType::Data) ++direct;
            else if(type==ProtocolV2::PacketType::Start) ++starts;
            else if(type==ProtocolV2::PacketType::End) ++ends;
        }
        assert(direct==packets && repairs==8*((packets+15)/16));
        assert(starts>=1 && ends==1 && markers>=1);
        assert(legacy_starts==1 && legacy_ends==1 && legacy_data==packets);
        DemoConsoleApp rx;rx.rx_no_ack_mode_=true;rx.manager_.send_success=true;rx.manager_.value.state=RadioState::RxListening;
        for(const auto& frame:tx.manager_.sent_frames)rx.processRxPayload(frame.data(),frame.size());
        assert(rx.manager_.send_attempts==0 && rx.receiver_.complete() && rx.saved_rx_file_count_==1);
        assert(readFile(rx.incoming_file_.final_name)==bytes);
        assert(std::filesystem::path(rx.incoming_file_.final_name).extension()==".PDF");
        if(length){
            // Drop a direct symbol, retaining FEC and excluding the fallback pass.
            // Then suppress all repair symbols too, or change the byte on every copy.
            for(unsigned impairment=0;impairment<3;++impairment){
                DemoConsoleApp sink;sink.rx_no_ack_mode_=true;sink.manager_.value.state=RadioState::RxListening;
                bool checksum_rejected=false;
                for(auto frame:tx.manager_.sent_frames){
                    if(frame[2]=='N' || (impairment!=0 && frame[2]=='P') || (frame[2]=='M' && frame[21]==2)) continue;
                    if(frame[2]=='G' && frame[4]==static_cast<uint8_t>(ProtocolV2::PacketType::Data)
                            && ProtocolV2::loadLe16(frame.data()+9)==0){
                        if(impairment<2) continue;
                        frame[12]^=1;
                    }
                    sink.processRxPayload(frame.data(),frame.size());
                    checksum_rejected |= sink.receiver_.error()==ProtocolV2::ErrorCode::CrcMismatch;
                }
                assert(sink.manager_.send_attempts==0);
                if(impairment==0){
                    assert(sink.receiver_.complete() && readFile(sink.incoming_file_.final_name)==bytes);
                } else {
                    assert(!sink.receiver_.complete() && sink.saved_rx_file_count_==0);
                    ProtocolV2::Frame reply{};
                    if(impairment==1){
                        // A silent receiver releases incomplete FEC state after inactivity.
                        const auto deadline = sink.receiver_.lastActivityMs()+ProtocolV2::kReceiverInactivityTimeoutMs;
                        assert(sink.receiver_.tick(deadline-1,reply)==ReliableTransferV2::ReceiverEvent::Ignored);
                        assert(sink.receiver_.tick(deadline,reply)==ReliableTransferV2::ReceiverEvent::TimedOut);
                        assert(!sink.receiver_.active()&&!sink.incoming_file_.file);
                        assert(!std::filesystem::exists(buildFilePath("rf3_gap_cache.part")));
                    } else assert(checksum_rejected);
                    if (sink.receiver_.active()) assert(sink.commandStop());
                    assert(!sink.receiver_.active());
                    assert(sink.sendProtocolResponseLocked(reply) && sink.manager_.send_attempts==0);
                }
            }
        }
        // Reliable START in silent mode is ignored, with no READY/error RF reply.
        DemoConsoleApp wrong;wrong.rx_no_ack_mode_=true;wrong.manager_.send_success=true;wrong.manager_.value.state=RadioState::RxListening;
        auto reliable=tx.manager_.sent_frames.front();reliable[2]='2';wrong.processRxPayload(reliable.data(),reliable.size());
        assert(!wrong.receiver_.active() && wrong.manager_.send_attempts==0);
        // Repeated silent RX preserves a partial; changing its mode requires Stop.
        if(length){
            DemoConsoleApp partial;partial.rx_no_ack_mode_=true;partial.rx_no_ack_requested_=true;
            partial.manager_.value.state=RadioState::RxListening;
            auto first=tx.manager_.sent_frames.front();partial.processRxPayload(first.data(),first.size());
            const auto id=partial.receiver_.transferId();assert(partial.commandRx());
            partial.rx_no_ack_requested_=false;assert(!partial.commandRx());assert(partial.receiver_.transferId()==id);
            assert(partial.commandStop());assert(partial.commandRx());assert(!partial.rx_no_ack_mode_);
        }
        DemoConsoleApp broken;broken.manager_.value.state=RadioState::Standby;
        auto failed_report=report;assert(!sendDataFileNoAck(broken.manager_,buildFilePath(name).c_str(),&stop,&failed_report));
        assert(broken.manager_.send_attempts==1 && failed_report.state==ReliableTransferV2::SenderState::Failed);
        ++cases;
    }
    std::cerr<<"Actual no-ACK sender and production silent receiver/CRC/publication/loss/timeout/mode-guard checks passed ("<<cases<<" file boundaries) for "<<HardwareProfile::kSelectedName<<'\n';
}
'''
    return cpp


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--compiler", default=r"C:\msys64\ucrt64\bin\g++.exe")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    legacy = (root / "test/fixtures/legacy_v2/include/protocol_v2.hpp").read_text(encoding="utf-8")
    (out / "legacy_protocol_v2.hpp").write_text(legacy.replace("ProtocolV2", "LegacyProtocolV2"), encoding="utf-8")
    source = out / "no_ack_runtime.cpp"
    source.write_text(harness(root), encoding="utf-8")
    env = os.environ.copy()
    env["PATH"] = str(Path(args.compiler).parent) + os.pathsep + env["PATH"]
    for profile, pins in ((2, (27,5,26)),):
        binary = out / f"no_ack_profile_{profile}.exe"
        flags = [f"-DRF3_HARDWARE_PROFILE_ID={profile}", "-DRF3_BLUETOOTH_CONTROL_ENABLED=1", "-DRF3_USB_EBYTE=1"]
        flags += [f"-DNRF24_{key}_PIN={value}" for key,value in zip(("CE","CSN","IRQ","SCK","MOSI","MISO"), (*pins,18,23,19))]
        subprocess.run([args.compiler,"-std=c++17","-Wall","-Wextra","-Werror","-pedantic","-pthread","-I",str(root/"include"),*flags,str(source),"-o",str(binary)],check=True,env=env)
        subprocess.run([str(binary),str(out/f"files_{profile}")],check=True,env=env)


if __name__ == "__main__":
    main()

