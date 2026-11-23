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
int main(int argc,char** argv){
    assert(argc==2);file_root=argv[1];std::filesystem::create_directories(file_root);
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
        assert(tx.manager_.sent_frames.size()==ProtocolV2::packetCountForSize(length)+2);
        // A Stop arriving before the first packet still retains No ACK in
        // the terminal report consumed by STATUS.
        DemoConsoleApp cancelled;cancelled.manager_.value.state=RadioState::Standby;
        auto cancelled_report=report;std::atomic_bool cancel_now{true};
        assert(!sendDataFileNoAck(cancelled.manager_,buildFilePath(name).c_str(),&cancel_now,&cancelled_report));
        assert(cancelled_report.no_ack && cancelled_report.state==ReliableTransferV2::SenderState::Cancelled);

        DemoConsoleApp rx;rx.rx_no_ack_mode_=true;rx.manager_.send_success=true;rx.manager_.value.state=RadioState::RxListening;
        for(const auto& frame:tx.manager_.sent_frames)rx.processRxPayload(frame.data(),frame.size());
        assert(rx.manager_.send_attempts==0 && rx.receiver_.complete() && rx.saved_rx_file_count_==1);
        assert(readFile(rx.incoming_file_.final_name)==bytes);
        assert(std::filesystem::path(rx.incoming_file_.final_name).extension()==".PDF");
        for(bool corrupt:{false,true}){
            if(length==0)continue;
            DemoConsoleApp failed;failed.rx_no_ack_mode_=true;failed.manager_.send_success=true;failed.manager_.value.state=RadioState::RxListening;
            for(size_t n=0;n<tx.manager_.sent_frames.size();++n){
                auto f=tx.manager_.sent_frames[n];if(!corrupt&&n==1)continue;if(corrupt&&n==1)f[12]^=1;
                failed.processRxPayload(f.data(),f.size());
            }
            assert(!failed.receiver_.complete() && failed.saved_rx_file_count_==0 && failed.manager_.send_attempts==0);
            ProtocolV2::Frame response{};failed.receiver_.tick(failed.receiver_.lastActivityMs()+ProtocolV2::kReceiverInactivityTimeoutMs,response);
            assert(!failed.receiver_.active());
            assert(failed.sendProtocolResponseLocked(response) && failed.manager_.send_attempts==0);
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
    for profile, pins in ((1, (17,27,16)), (2, (27,5,26))):
        binary = out / f"no_ack_profile_{profile}.exe"
        flags = [f"-DRF3_HARDWARE_PROFILE_ID={profile}", "-DRF3_BLUETOOTH_CONTROL_ENABLED=1"]
        flags += [f"-DNRF24_{key}_PIN={value}" for key,value in zip(("CE","CSN","IRQ","SCK","MOSI","MISO"), (*pins,18,23,19))]
        subprocess.run([args.compiler,"-std=c++17","-Wall","-Wextra","-Werror","-pedantic","-pthread","-I",str(root/"include"),*flags,str(source),"-o",str(binary)],check=True,env=env)
        subprocess.run([str(binary),str(out/f"files_{profile}")],check=True,env=env)


if __name__ == "__main__":
    main()
