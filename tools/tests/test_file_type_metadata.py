"""Execute real transfer service/codec/receiver with production file sink methods.
The radio/task adapter is native-only; no device access occurs.
"""
from pathlib import Path
import argparse
import hashlib
import importlib.util
import os
import subprocess
import shutil


def harness(root: Path) -> str:
    spec=importlib.util.spec_from_file_location("rxguard",root/"tools/tests/test_rx_command_guard.py")
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    cpp=module.harness(root)
    cpp=cpp[:cpp.index("ReliableTransferV2::ReceiverEvent feed(")]
    main=(root/"src/main.cpp").read_text(encoding="utf-8")
    cpp=cpp.replace('bool takeLoop(TickType_t){','bool takeLoop(TickType_t=100){')
    cpp=cpp.replace('#include "hardware_profile.hpp"','#include "hardware_profile.hpp"\n#include "legacy_protocol_v2.hpp"')
    incoming=main[main.index('struct IncomingFileStorage {'):main.index('\n};',main.index('struct IncomingFileStorage {'))+4]
    builder=main[main.index('std::string buildReceivedFileName('):main.index('\nbool resolveFile(')]
    # Native resolver/parser adapters only locate controlled test files; the
    # complete production TX handler below supplies the tested preflight order.
    adapters = main[main.index('std::string trimAscii('):main.index('std::string uppercaseCopy(')]
    adapters += main[main.index('bool resolveFile('):main.index('std::vector<FileInfo> listFiles(')]
    adapters += 'bool parseLoopCountToken(std::string_view text,bool& infinite,uint32_t& count){infinite=text=="INF";count=0;return infinite;}\n'
    at=cpp.index('class DemoConsoleApp {')
    cpp=cpp[:at]+incoming+'\nconstexpr uint16_t kMaxReceivedFileCollisionIndex=999;\n'+builder+adapters+cpp[at:]
    cpp=cpp.replace('    TestSink sink_;\n    ReliableTransferV2::ReceiverSession receiver_{&sink_,TestSink::callbacks()};','    ReliableTransferV2::ReceiverSession receiver_{this,incomingStorageCallbacks()};')
    cpp=cpp.replace('    struct Incoming {std::FILE* file=nullptr;std::string partial_name;} incoming_file_;','    IncomingFileStorage incoming_file_{};')
    sink_methods=main[main.index('    static ReliableTransferV2::SinkPrepareResult prepareIncomingStorageEntry('):main.index('    void discardIncomingFileTransfer(')]
    cpp=cpp.rstrip()[:-2]+sink_methods+main[main.index('    bool commandTx('):main.index('    bool commandRemote(')]+'\n};\n'
    tests=r"""
class BytesSource final : public FileTransfer::IDataSource {
public:
    std::vector<uint8_t> bytes;size_t position=0;unsigned resets=0,reads=0;
    bool reset()override{++resets;position=0;return true;}
    FileTransfer::ReadResult read(uint8_t* out,size_t capacity)override{
        ++reads;const auto count=std::min(capacity,bytes.size()-position);std::copy_n(bytes.begin()+position,count,out);position+=count;
        return {count,position==bytes.size()?FileTransfer::ReadState::EndOfFile:FileTransfer::ReadState::MoreDataMayFollow};
    }
};
uint32_t ids(void*){static uint32_t id=0x11220000;return ++id;}
std::vector<uint8_t> content(size_t length){std::vector<uint8_t> bytes(length);for(size_t i=0;i<length;++i)bytes[i]=static_cast<uint8_t>(i*37+11);return bytes;}
std::vector<uint8_t> readFile(const std::string& name){std::ifstream file(buildFilePath(name),std::ios::binary);return {(std::istreambuf_iterator<char>(file)),{}};}
ProtocolV2::Packet decoded(const ProtocolV2::Frame& frame){ProtocolV2::Packet packet{};assert(ProtocolV2::decode(frame.data(),frame.size(),packet)==ProtocolV2::DecodeStatus::Ok);return packet;}
void wireChecks(){
    using namespace ProtocolV2;
    Packet start{};start.type=PacketType::Start;start.transfer_id=0x78563412;start.total_size=21;start.total_packets=2;start.crc32=0xa1b2c3d4;
    Frame legacy{};assert(encode(start,legacy));
    const Frame expected{'R','F','2',2,1,0x12,0x34,0x56,0x78,21,0,0,0,2,0,0,0,0xd4,0xc3,0xb2,0xa1,0,0,0,0,0,0,0,0,0,0,0};
    assert(legacy==expected);LegacyProtocolV2::Packet old{};assert(LegacyProtocolV2::decode(legacy.data(),legacy.size(),old)==LegacyProtocolV2::DecodeStatus::Ok);
    FileTypeMetadata::Extension extension{};assert(FileTypeMetadata::derive("a.PDF",extension));start.extension=extension;start.flags=1;
    Frame extended{};assert(encode(start,extended));assert(extended[21]==1&&extended[22]=='.'&&extended[23]=='P');
    assert(LegacyProtocolV2::decode(extended.data(),extended.size(),old)==LegacyProtocolV2::DecodeStatus::InvalidField);
    assert(FileTypeMetadata::view(decoded(extended).extension)==".PDF");
    for(const auto suffix:{".pdf",".PDF",".123456789",".h_-1"}){assert(FileTypeMetadata::derive(std::string("a")+suffix,extension));assert(FileTypeMetadata::canonical(extension,false));}
    for(const auto suffix:{".",".1234567890",".part",".PART",".pArt",".p/f",".p\\f",".p:f",".p+f",".p f",".p..f",".\x7f",".\xc3\xa9"})assert(!FileTypeMetadata::valid(suffix));
    for(uint8_t flags:{uint8_t{2},uint8_t{3},uint8_t{255}}){auto bad=extended;bad[21]=flags;Packet out;assert(decode(bad.data(),bad.size(),out)==DecodeStatus::InvalidField);}
    {auto bad=extended;bad[31]='x';Packet out;assert(decode(bad.data(),bad.size(),out)==DecodeStatus::InvalidField);}
    {auto bad=extended;bad[21]=0;Packet out;assert(decode(bad.data(),bad.size(),out)==DecodeStatus::NonzeroReserved);}
    {auto bad=legacy;bad[21]=1;Packet out;assert(decode(bad.data(),bad.size(),out)==DecodeStatus::InvalidField);}
    {Packet malformed=start;malformed.extension.fill(0);std::copy_n(".part",5,malformed.extension.begin());Frame out;assert(!encode(malformed,out));}
    for(const std::string unsafe:{".part",".PART","..",".p/f",".p\\f",".p:f",".p+f",".p f",".\x7f",".\xc3\xa9","."}){
        auto bad=legacy;bad[21]=1;std::copy(unsafe.begin(),unsafe.end(),bad.begin()+22);Packet out;assert(decode(bad.data(),bad.size(),out)==DecodeStatus::InvalidField);
        DemoConsoleApp victim;Frame reply{};(void)victim.receiver_.onFrame(bad.data(),bad.size(),1,reply);assert(victim.receiver_.state()==ReliableTransferV2::ReceiverState::Idle&&victim.incoming_file_.file==nullptr&&victim.incoming_file_.final_name.empty());
    }
    assert(FileTypeMetadata::derive("archive.tar.gz",extension)&&FileTypeMetadata::view(extension)==".gz");
    assert(FileTypeMetadata::derive("extensionless",extension)&&FileTypeMetadata::empty(extension));
    static_assert(kFrameSize==32&&kDataPayloadCapacity==20&&kMaxFileSize==1310720);
}
void preflight(){
    DemoConsoleApp app;app.manager_.value.state=RadioState::Standby;app.manager_.value.power_level=2;
    const std::string long_name="payload.1234567890";std::ofstream(buildFilePath(long_name),std::ios::binary).write("safe",4);
    assert(!app.commandStoredName({"TXHEX",HostStorageProtocol::encodeName(long_name)},CommandOrigin::Local));
    assert(app.replies.back().find("unsupported_file_extension")!=std::string::npos);
    assert(!app.commandTx({"TX",long_name},CommandOrigin::Local));
    assert(!app.commandTx({"TX","LOOP","INF",long_name},CommandOrigin::Local));
    assert(app.stop_loop_calls_==0&&!app.loop_config_.active&&copyTransferReport(app.last_tx_report_).state==ReliableTransferV2::SenderState::Idle);
    for(const auto name:{"payload.1234567890","payload.part","payload.PART","payload.p+f","payload."}){
        BytesSource source;source.bytes=content(41);FileTransfer::Service service(&ids);FileTransfer::TransferMetadata metadata;assert(metadata.logical_filename.set(name));
        const auto rejected=service.startTransfer(source,metadata,0);assert(rejected.code==FileTransfer::StartCode::InvalidExtension);assert(source.resets==0&&source.reads==0);
        assert(service.transportSender().state()==ReliableTransferV2::SenderState::Idle);
    }
}
void transfer(std::string logical,size_t length,bool collisions,bool corrupt=false){
    DemoConsoleApp app;app.manager_.value.state=RadioState::RxListening;app.manager_.value.power_level=2;
    BytesSource source;source.bytes=content(length);FileTransfer::Service service(&ids);FileTransfer::TransferMetadata metadata;assert(metadata.logical_filename.set(logical));
    const auto started=service.startTransfer(source,metadata,0);assert(started);auto& sender=service.transportSender();ProtocolV2::Frame outbound{},reply{};
    assert(sender.outboundFrame(outbound));const auto start=decoded(outbound);
    FileTypeMetadata::Extension expected{};assert(FileTypeMetadata::derive(logical,expected));const auto suffix=FileTypeMetadata::view(expected);
    assert(start.flags==(suffix.empty()?0:1)&&start.extension==expected);
    const auto base=buildReceivedFileName(started.transfer_id,false,0,suffix);
    if(collisions)std::ofstream(buildFilePath(base),std::ios::binary).write("OLD",3);
    assert(sender.noteFrameSent(0));assert(app.receiver_.onFrame(outbound.data(),outbound.size(),1,reply)==ReliableTransferV2::ReceiverEvent::ResponseReady);
    const auto opened=app.incoming_file_.file;const auto selected=app.incoming_file_.final_name;const auto original_partial=app.incoming_file_.partial_name;
    assert(opened&&app.receiver_.fileExtension()==suffix&&FileTypeMetadata::valid(std::filesystem::path(selected).extension().string()));
    // READY loss: sender retries byte-identical extension START; receiver keeps sink.
    (void)sender.tick(501);ProtocolV2::Frame retried{};assert(sender.outboundFrame(retried)&&retried==outbound);assert(sender.noteFrameSent(501));
    assert(app.receiver_.onFrame(retried.data(),retried.size(),502,reply)==ReliableTransferV2::ReceiverEvent::ResponseReady);
    assert(app.incoming_file_.file==opened&&app.incoming_file_.final_name==selected);
    // Same-ID valid but different type must not retarget the active sink.
    auto spoof=start;spoof.flags=1;assert(FileTypeMetadata::derive("x.exe",spoof.extension));ProtocolV2::Frame spoofed{};assert(ProtocolV2::encode(spoof,spoofed));
    assert(app.receiver_.onFrame(spoofed.data(),spoofed.size(),503,reply)==ReliableTransferV2::ReceiverEvent::ResponseReady);assert(!decoded(reply).accepted&&decoded(reply).error==ProtocolV2::ErrorCode::Busy);assert(app.incoming_file_.file==opened&&app.receiver_.fileExtension()==suffix);
    // Feed the correct READY again to the actual sender.
    (void)app.receiver_.onFrame(retried.data(),retried.size(),504,reply);(void)sender.onFrame(reply.data(),reply.size(),505);
    uint64_t now=506;bool made_collision=false,flipped=false,repeated_rx=false;
    while(!sender.terminal()&&now<100000){
        assert(sender.outboundFrame(outbound));auto packet=decoded(outbound);
        if(packet.type==ProtocolV2::PacketType::End&&collisions&&!made_collision){std::ofstream(buildFilePath(selected),std::ios::binary).write("OLD2",4);made_collision=true;}
        if(corrupt&&!flipped&&packet.type==ProtocolV2::PacketType::Data){packet.payload[0]^=0xff;assert(ProtocolV2::encode(packet,outbound));flipped=true;}
        assert(sender.noteFrameSent(now));const auto event=app.receiver_.onFrame(outbound.data(),outbound.size(),++now,reply);
        if(packet.type==ProtocolV2::PacketType::Data&&!repeated_rx){const auto before=app.receiver_.acceptedBytes();const auto* partial=app.incoming_file_.file;assert(app.commandRx());assert(app.receiver_.acceptedBytes()==before&&app.incoming_file_.file==partial&&app.receiver_.fileExtension()==suffix);repeated_rx=true;}
        (void)event;(void)sender.onFrame(reply.data(),reply.size(),++now);
    }
    assert(sender.terminal());
    if(corrupt){assert(sender.state()==ReliableTransferV2::SenderState::Failed);assert(app.receiver_.error()==ProtocolV2::ErrorCode::CrcMismatch);assert(!std::filesystem::exists(buildFilePath(original_partial)));return;}
    assert(sender.state()==ReliableTransferV2::SenderState::Completed&&sender.peerComplete());assert(app.receiver_.complete());
    assert(readFile(app.incoming_file_.final_name)==source.bytes);assert(app.receiver_.calculatedCrc32()==ProtocolV2::calculateCrc32(source.bytes.data(),source.bytes.size()));
    assert(std::filesystem::path(app.incoming_file_.final_name).extension().string()==(suffix.empty()?".bin":std::string(suffix)));
    if(collisions){assert(readFile(base)==std::vector<uint8_t>({'O','L','D'}));assert(readFile(selected)==std::vector<uint8_t>({'O','L','D','2'}));assert(app.incoming_file_.final_name!=selected);}
    // Type-changing stale START cannot retarget the terminal request either.
    assert(app.receiver_.onFrame(spoofed.data(),spoofed.size(),++now,reply)==ReliableTransferV2::ReceiverEvent::ResponseReady);assert(!decoded(reply).accepted);assert(app.receiver_.fileExtension()==suffix&&app.receiver_.complete());
    std::cout<<"{\"logical\":\""<<logical<<"\",\"saved\":\""<<app.incoming_file_.final_name<<"\",\"bytes\":"<<length<<",\"crc32\":\""<<HostStatusProtocol::hex32(app.receiver_.calculatedCrc32())<<"\",\"peer_complete\":true}"<<'\n';
}
int main(int argc,char** argv){
    assert(argc==2);file_root=argv[1];std::filesystem::create_directories(file_root);wireChecks();preflight();
    transfer("report.pdf",43,true);transfer("caps.PDF",0,false);transfer("max.123456789",21,false);transfer("archive.tar.gz",57,false);transfer("noextension",41,false);transfer("badcrc.pdf",23,false,true);
    std::cerr<<"Extension codec/service/production-sink/retry/CRC/collision/legacy checks passed for "<<HardwareProfile::kSelectedName<<'\n';
}
"""
    return cpp+tests


def main() -> None:
    parser=argparse.ArgumentParser();parser.add_argument("--output",type=Path,required=True);parser.add_argument("--legacy-root",type=Path,default=Path(__file__).resolve().parents[2]/"test/fixtures/legacy_v2");parser.add_argument("--compiler",default=shutil.which("g++") or r"C:\msys64\ucrt64\bin\g++.exe");args=parser.parse_args()
    root=Path(__file__).resolve().parents[2];out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    legacy=(args.legacy_root/"include/protocol_v2.hpp").read_bytes();(out/"legacy_protocol_v2.hpp").write_text(legacy.decode().replace("ProtocolV2","LegacyProtocolV2"),encoding="utf-8")
    (out/"Legacy_Source_SHA256.txt").write_text(hashlib.sha256(legacy).hexdigest()+"  "+str(args.legacy_root/"include/protocol_v2.hpp")+"\nNamespace-only adaptation for simultaneous native comparison.\n")
    source=out/"production_file_type_harness.cpp";source.write_text(harness(root),encoding="utf-8")
    env=os.environ.copy();env["PATH"]=str(Path(args.compiler).parent)+os.pathsep+env["PATH"]
    for profile,pins in ((1,(17,27,16)),(2,(27,5,26))):
        binary=out/f"file_type_profile_{profile}.exe";flags=["-DRF3_BLUETOOTH_CONTROL_ENABLED=1",f"-DRF3_HARDWARE_PROFILE_ID={profile}",f"-DNRF24_CE_PIN={pins[0]}",f"-DNRF24_CSN_PIN={pins[1]}",f"-DNRF24_IRQ_PIN={pins[2]}","-DNRF24_SCK_PIN=18","-DNRF24_MOSI_PIN=23","-DNRF24_MISO_PIN=19"]
        subprocess.run([args.compiler,"-std=c++17","-Wall","-Wextra","-Werror","-pedantic","-pthread","-I",str(root/"include"),*flags,str(source),"-o",str(binary)],check=True,env=env)
        run=subprocess.run([str(binary),str(out/f"files_profile_{profile}")],check=True,env=env,capture_output=True);(out/f"saved_files_profile_{profile}.txt").write_bytes(run.stdout);print(run.stderr.decode(errors="replace"),end="")
if __name__=="__main__":main()
