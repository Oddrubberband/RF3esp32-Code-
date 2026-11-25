"""Exercise production receive dispatch/storage and real sender/service without hardware."""
import argparse
import importlib.util
import os
from pathlib import Path
import subprocess


def harness(root):
    spec = importlib.util.spec_from_file_location("oneway", root / "tools/tests/test_no_ack_runtime.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    cpp = module.harness(root)
    cpp = cpp[:cpp.index("int main(int argc,char** argv){")]
    cpp += r'''
void deliver(DemoConsoleApp& app, ReliableTransferV2::SenderSession& sender, uint64_t& now,
             bool reply=true) {
    ProtocolV2::Frame frame{};assert(sender.outboundFrame(frame));assert(sender.noteFrameSent(++now));
    const auto before=app.manager_.sent_frames.size();app.processRxPayload(frame.data(),frame.size());
    assert(app.manager_.sent_frames.size()==before+1);
    if(reply){const auto& response=app.manager_.sent_frames.back();(void)sender.onFrame(response.data(),response.size(),++now);}
}
void interrupted(size_t length,uint32_t prefix,bool timeout,int damage,bool lose_ack=false) {
    DemoConsoleApp app;app.manager_.send_success=true;app.manager_.value.state=RadioState::RxListening;
    BytesSource source;source.bytes=content(length);FileTransfer::Service service(&ids);
    FileTransfer::TransferMetadata metadata;assert(metadata.logical_filename.set("resume.PDF"));
    uint64_t now=1;assert(service.startTransfer(source,metadata,now));auto& sender=service.transportSender();
    const auto old_id=sender.transferId();deliver(app,sender,now);
    for(uint32_t i=0;i<prefix;++i)deliver(app,sender,now,!(lose_ack&&i+1==prefix));
    const auto accepted=app.receiver_.acceptedBytes();assert(app.receiver_.expectedSequence()==prefix);
    const auto partial=app.incoming_file_.partial_name;
    // Exhaust the sender budget without delivering retries, as in an outage.
    ProtocolV2::Frame frame{};
    if(sender.outboundFrame(frame))assert(sender.noteFrameSent(++now));
    for(unsigned tries=0;!sender.terminal()&&tries<30;++tries){
        now+=1001;(void)sender.tick(now);
        if(sender.outboundFrame(frame))assert(sender.noteFrameSent(now));
    }
    assert(sender.state()==ReliableTransferV2::SenderState::Failed);
    if(timeout){
        ProtocolV2::Frame response{};
        assert(app.receiver_.tick(app.receiver_.lastActivityMs()+20001,response)==ReliableTransferV2::ReceiverEvent::TimedOut);
        app.active_api_receive_id_=0;
        assert(!app.receiver_.active()&&app.receiver_.resumable()&&!app.incoming_file_.file);
        assert(std::filesystem::file_size(buildFilePath(partial))==accepted);
    }
    if(damage){
        assert(timeout);
        if(damage==1){std::ofstream f(buildFilePath(partial),std::ios::binary|std::ios::app);f.put('x');}
        if(damage==2){std::ofstream f(buildFilePath(partial),std::ios::binary|std::ios::trunc);f.put('x');}
        if(damage==3){std::fstream f(buildFilePath(partial),std::ios::binary|std::ios::in|std::ios::out);f.put('x');}
        if(damage==4)std::filesystem::remove(buildFilePath(partial));
    }
    metadata.resume_from=old_id;
    assert(service.startTransfer(source,metadata,++now));assert(sender.transferId()!=old_id);
    deliver(app,sender,now);
    assert(sender.acknowledgedBytes()==(damage?0:accepted));
    assert(sender.currentSequence()==(damage?0:prefix));
    if(!damage)assert(app.incoming_file_.partial_name==partial);
    {
        ProtocolV2::Packet old_start{};old_start.type=ProtocolV2::PacketType::Start;old_start.transfer_id=old_id;
        old_start.total_size=static_cast<uint32_t>(length);old_start.total_packets=ProtocolV2::packetCountForSize(length);
        old_start.crc32=sender.crc32();old_start.flags=1;assert(FileTypeMetadata::derive("resume.PDF",old_start.extension));
        assert(ProtocolV2::encode(old_start,frame));const auto id=app.receiver_.transferId();
        app.processRxPayload(frame.data(),frame.size());assert(app.receiver_.transferId()==id);
    }
    // Frames from the old attempt cannot append or cancel the new attempt.
    ProtocolV2::Packet stale{};stale.type=ProtocolV2::PacketType::Cancel;stale.transfer_id=old_id;stale.error=ProtocolV2::ErrorCode::Cancelled;
    assert(ProtocolV2::encode(stale,frame));const auto before=app.receiver_.acceptedBytes();
    app.processRxPayload(frame.data(),frame.size());assert(app.receiver_.acceptedBytes()==before&&app.receiver_.active());
    unsigned delivered=0;
    while(!sender.terminal()){deliver(app,sender,now);assert(++delivered<100000);}
    assert(sender.peerComplete()&&app.receiver_.complete()&&app.saved_rx_file_count_==1);
    assert(readFile(app.incoming_file_.final_name)==source.bytes);
    assert(app.incoming_file_.partial_name.empty());
    assert(std::filesystem::path(app.incoming_file_.final_name).extension()==".PDF");
}
void cleanupAndIdentity(){
    for(int change=0;change<4;++change){
        DemoConsoleApp app;app.manager_.send_success=true;app.manager_.value.state=RadioState::RxListening;
        BytesSource first;first.bytes=content(43);FileTransfer::Service service(&ids);
        FileTransfer::TransferMetadata meta;assert(meta.logical_filename.set("one.pdf"));uint64_t now=1;
        assert(service.startTransfer(first,meta,now));auto& sender=service.transportSender();deliver(app,sender,now);deliver(app,sender,now);
        ProtocolV2::Frame response{};assert(app.receiver_.tick(app.receiver_.lastActivityMs()+20001,response)==ReliableTransferV2::ReceiverEvent::TimedOut);
        app.active_api_receive_id_=0;const auto partial=app.incoming_file_.partial_name;
        if(change==0){assert(app.commandStop());assert(!app.receiver_.resumable());assert(!std::filesystem::exists(buildFilePath(partial)));continue;}
        BytesSource other;other.bytes=first.bytes;
        if(change==1)other.bytes.push_back(0);
        if(change==2)other.bytes[0]^=1;
        if(change==3)assert(meta.logical_filename.set("one.txt"));
        meta.resume_from=sender.transferId();
        FileTransfer::Service next(&ids);assert(next.startTransfer(other,meta,++now));auto& tx=next.transportSender();deliver(app,tx,now);
        assert(tx.acknowledgedBytes()==0&&app.receiver_.expectedSequence()==0);
        while(!tx.terminal())deliver(app,tx,now);
        assert(tx.peerComplete()&&readFile(app.incoming_file_.final_name)==other.bytes);
    }
}
void completedRetry(){
    for(int damage=0;damage<3;++damage){
        DemoConsoleApp app;app.manager_.send_success=true;app.manager_.value.state=RadioState::RxListening;
        BytesSource source;source.bytes=content(43);FileTransfer::Service service(&ids);
        FileTransfer::TransferMetadata meta;assert(meta.logical_filename.set("complete.pdf"));uint64_t now=1;
        assert(service.startTransfer(source,meta,now));auto& tx=service.transportSender();deliver(app,tx,now);
        while(tx.state()!=ReliableTransferV2::SenderState::WaitingForComplete)deliver(app,tx,now);
        deliver(app,tx,now,false);assert(app.receiver_.complete());const auto saved=app.incoming_file_.final_name;
        ProtocolV2::Frame frame{};
        for(unsigned tries=0;!tx.terminal()&&tries<30;++tries){now+=1001;(void)tx.tick(now);if(tx.outboundFrame(frame))assert(tx.noteFrameSent(now));}
        assert(tx.state()==ReliableTransferV2::SenderState::Failed);
        if(damage==1)std::filesystem::remove(buildFilePath(saved));
        if(damage==2){std::fstream f(buildFilePath(saved),std::ios::binary|std::ios::in|std::ios::out);f.put('x');}
        meta.resume_from=tx.transferId();
        assert(service.startTransfer(source,meta,++now));deliver(app,tx,now);
        assert(tx.acknowledgedBytes()==(damage?0:43));
        while(!tx.terminal())deliver(app,tx,now);
        assert(tx.peerComplete()&&readFile(app.incoming_file_.final_name)==source.bytes);
        assert(app.saved_rx_file_count_==(damage?2:1));
        if(!damage)assert(app.incoming_file_.final_name==saved);
    }
}
void invalidOffset(){
    BytesSource source;source.bytes=content(43);FileTransfer::Service service(&ids);
    FileTransfer::TransferMetadata meta;assert(meta.logical_filename.set("invalid.pdf"));assert(service.startTransfer(source,meta,0));
    auto& tx=service.transportSender();ProtocolV2::Frame frame{};assert(tx.outboundFrame(frame));assert(tx.noteFrameSent(1));
    ProtocolV2::Packet ready{};ready.type=ProtocolV2::PacketType::Ready;ready.transfer_id=tx.transferId();ready.accepted=true;ready.expected_sequence=4;
    assert(ProtocolV2::encode(ready,frame));(void)tx.onFrame(frame.data(),frame.size(),2);
    assert(tx.state()==ReliableTransferV2::SenderState::Failed&&tx.error()==ProtocolV2::ErrorCode::UnexpectedSequence);
}
void boundedHistory(){
    DemoConsoleApp app;app.manager_.send_success=true;app.manager_.value.state=RadioState::RxListening;
    BytesSource source;source.bytes=content(43);FileTransfer::Service service(&ids);
    FileTransfer::TransferMetadata meta;assert(meta.logical_filename.set("history.pdf"));uint64_t now=1;
    assert(service.startTransfer(source,meta,now));auto& tx=service.transportSender();ProtocolV2::Frame start{};assert(tx.outboundFrame(start));
    const auto original=decoded(start);deliver(app,tx,now);deliver(app,tx,now);
    constexpr uint32_t attempts=64;
    for(uint32_t i=1;i<=attempts;++i){
        auto packet=original;packet.resume_from=original.transfer_id;packet.flags|=ProtocolV2::kResumeStartFlag;packet.transfer_id=original.transfer_id+i;assert(ProtocolV2::encode(packet,start));app.processRxPayload(start.data(),start.size());
        const auto response=decoded(app.manager_.sent_frames.back());
        assert(response.accepted&&response.expected_sequence==1&&app.receiver_.transferId()==packet.transfer_id);
    }
    for(uint32_t i=attempts-32;i<attempts;++i){
        auto packet=original;packet.transfer_id+=i;assert(ProtocolV2::encode(packet,start));
        app.processRxPayload(start.data(),start.size());
        assert(app.receiver_.transferId()==original.transfer_id+attempts);
    }
    assert(app.receiver_.acceptedBytes()==20&&app.commandStop()&&!app.receiver_.resumable());
}

void repeatAndLateStop(){
    for(unsigned cancel_reply:{0u,1u,2u}){
        DemoConsoleApp rx;
        rx.manager_.send_success=true;
        rx.manager_.value.state=RadioState::RxListening;
        BytesSource source;source.bytes=content(43);
        FileTransfer::Service service(&ids);
        FileTransfer::TransferMetadata meta;
        assert(meta.logical_filename.set("again.bin"));
        uint64_t now=100;
        for(unsigned run=0;run<2;++run){
            assert(service.startTransfer(source,meta,now));
            auto& tx=service.transportSender();
            unsigned data=0, cancel_count=0;
            while(!tx.terminal()){
                ProtocolV2::Frame frame{};assert(tx.outboundFrame(frame));
                const auto packet=decoded(frame);
                if(packet.type==ProtocolV2::PacketType::Data)++data;
                if(run==1 && cancel_reply && packet.type==ProtocolV2::PacketType::End){
                    deliver(rx,tx,now,false);
                    assert(rx.receiver_.complete() && service.cancelTransfer(tx.transferId(),++now));
                } else if(packet.type==ProtocolV2::PacketType::Cancel){
                    ++cancel_count;
                    assert(cancel_count<20);
                    deliver(rx,tx,now,cancel_reply==1);
                    if(cancel_reply==2){now+=1001;(void)tx.tick(now);}
                } else {
                    deliver(rx,tx,now);
                }
            }
            assert(data==3 && rx.saved_rx_file_count_==run+1);
            assert(readFile(rx.incoming_file_.final_name)==source.bytes);
            if(run==1 && cancel_reply==2) {
                assert(cancel_count>1 && tx.state()==ReliableTransferV2::SenderState::Failed);
                assert(tx.error()==ProtocolV2::ErrorCode::CancelUnconfirmed);
            } else {
                assert(tx.peerComplete() && tx.acknowledgedBytes()==43);
            }
        }
    }
}
int main(int argc,char** argv){
    assert(argc==2);file_root=argv[1];std::filesystem::create_directories(file_root);
    repeatAndLateStop();
    unsigned cases=0;
    for(size_t length:{0u,1u,20u,21u,43u,513u,1310720u}){
        const auto packets=ProtocolV2::packetCountForSize(length);
        for(uint32_t prefix:{0u,packets/2,packets})for(bool timeout:{false,true}){
            interrupted(length,prefix,timeout,0);++cases;
        }
    }
    for(int damage=1;damage<=4;++damage){interrupted(43,1,true,damage);++cases;}
    interrupted(43,1,false,0,true);interrupted(43,1,true,0,true);cleanupAndIdentity();completedRetry();invalidOffset();boundedHistory();
    std::cerr<<"Resume production dispatch/storage/sender checks passed: "<<cases+11
             <<" cases, active and timed-out restart, lost ACK, EOF, max-size, damaged partial, identity, Stop, CRC/publication: "
             <<HardwareProfile::kSelectedName<<'\n';
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
    source = out / "resume_runtime.cpp"
    source.write_text(harness(root), encoding="utf-8")
    env = os.environ.copy()
    env["PATH"] = str(Path(args.compiler).parent) + os.pathsep + env["PATH"]
    for profile, pins in ((1, (17,27,16)), (2, (27,5,26))):
        binary = out / f"resume_profile_{profile}.exe"
        flags = [f"-DRF3_HARDWARE_PROFILE_ID={profile}", "-DRF3_BLUETOOTH_CONTROL_ENABLED=1"]
        flags += [f"-DNRF24_{key}_PIN={value}" for key,value in zip(("CE","CSN","IRQ","SCK","MOSI","MISO"), (*pins,18,23,19))]
        subprocess.run([args.compiler,"-std=c++17","-O2","-Wall","-Wextra","-Werror","-pedantic","-pthread","-I",str(root/"include"),*flags,str(source),"-o",str(binary)],check=True,env=env)
        run = subprocess.run([str(binary),str(out/f"files_{profile}")],env=env,capture_output=True)
        (out/f"profile_{profile}.log").write_bytes(run.stdout + run.stderr)
        run.check_returncode()
        print(run.stderr.decode(errors="replace"), end="")


if __name__ == "__main__":
    main()
