#include <cassert>
#include <iostream>
#include <vector>
#include "no_ack_transfer.hpp"
#include "nrf24.hpp"
#include "fake_hal.hpp"

struct Source {
    std::vector<uint8_t> bytes;
    size_t offset = 0;
    unsigned resets = 0;
    bool fail_after_inspection = false;
    static bool reset(void* ctx) { auto& s=*static_cast<Source*>(ctx);s.offset=0;++s.resets;return true; }
    static ReliableTransferV2::ReadResult read(void* ctx,uint8_t* out,size_t capacity) {
        auto& s=*static_cast<Source*>(ctx);
        if(s.fail_after_inspection && s.resets>=2) return {0,ReliableTransferV2::ReadState::Error};
        size_t n=std::min(capacity,s.bytes.size()-s.offset);
        std::copy_n(s.bytes.data()+s.offset,n,out);s.offset+=n;
        return {n,s.offset==s.bytes.size()?ReliableTransferV2::ReadState::EndOfFile:ReliableTransferV2::ReadState::MoreDataMayFollow};
    }
};
struct Sink {
    std::vector<uint8_t> bytes;
    unsigned published=0,removed=0;
    static ReliableTransferV2::SinkPrepareResult prepare(void* ctx,uint32_t,uint32_t) {static_cast<Sink*>(ctx)->bytes.clear();return ReliableTransferV2::SinkPrepareResult::Ready;}
    static size_t write(void* ctx,const uint8_t* data,size_t n) {auto& b=static_cast<Sink*>(ctx)->bytes;b.insert(b.end(),data,data+n);return n;}
    static bool close(void*) {return true;}
    static bool publish(void* ctx) {++static_cast<Sink*>(ctx)->published;return true;}
    static bool remove(void* ctx) {++static_cast<Sink*>(ctx)->removed;return true;}
    static ReliableTransferV2::SinkCallbacks callbacks(){return {prepare,write,close,publish,remove};}
};
std::vector<ProtocolV2::Frame> frames(Source& source) {
    NoAckTransfer::Sender sender(&source,{Source::reset,Source::read});
    assert(sender.begin(77,"report.PDF"));
    std::vector<ProtocolV2::Frame> out;
    while(!sender.done()) {
        ProtocolV2::Frame frame{};assert(sender.frame(frame));out.push_back(frame);
        assert(sender.noteSent());assert(out.size()<=source.bytes.size()/20+3);
    }
    assert(sender.sentBytes()==source.bytes.size());
    ProtocolV2::Frame terminal{};assert(!sender.frame(terminal));
    return out;
}
void clean(size_t length) {
    Source source;for(size_t i=0;i<length;++i)source.bytes.push_back(static_cast<uint8_t>(i));
    auto wire=frames(source);assert(wire.size()==ProtocolV2::packetCountForSize(length)+2);
    Sink sink;ReliableTransferV2::ReceiverSession receiver(&sink,Sink::callbacks());
    uint64_t now=0;
    for(const auto& f:wire) {
        ProtocolV2::Frame adapted{},ignored_response{};ProtocolV2::Packet packet{};
        assert(NoAckTransfer::decode(f.data(),f.size(),packet,adapted));
        assert(receiver.onFrame(f.data(),f.size(),++now,ignored_response)==ReliableTransferV2::ReceiverEvent::Ignored);
        receiver.onFrame(adapted.data(),adapted.size(),++now,ignored_response);
        if(packet.type==ProtocolV2::PacketType::Data)
            receiver.onFrame(adapted.data(),adapted.size(),++now,ignored_response); // duplicate never appends twice
    }
    assert(receiver.complete() && sink.published==1 && sink.bytes==source.bytes);
    assert(receiver.fileExtension()==".PDF");
}
int main() {
    for(size_t length:{0u,1u,20u,21u,300u}) clean(length);
    Source source;source.bytes.assign(43,0xA5);auto wire=frames(source);
    for(bool corrupt:{false,true}) {
        Sink sink;ReliableTransferV2::ReceiverSession receiver(&sink,Sink::callbacks());uint64_t now=0;
        for(size_t i=0;i<wire.size();++i) {
            auto f=wire[i];if(!corrupt && i==1)continue;if(corrupt && i==1)f[12]^=1;
            ProtocolV2::Frame adapted{},response{};ProtocolV2::Packet packet{};
            assert(NoAckTransfer::decode(f.data(),f.size(),packet,adapted));
            receiver.onFrame(adapted.data(),adapted.size(),++now,response);
        }
        assert(!receiver.complete() && sink.published==0);
        ProtocolV2::Frame response{};receiver.tick(receiver.lastActivityMs()+ProtocolV2::kReceiverInactivityTimeoutMs,response);
        assert(!receiver.active() && sink.published==0 && sink.removed>0);
    }
    ProtocolV2::Packet ack{};ack.type=ProtocolV2::PacketType::Ack;ack.transfer_id=1;
    ProtocolV2::Frame rejected{};assert(!NoAckTransfer::encode(ack,rejected));
    auto wrong=wire.front();wrong[2]='2';ProtocolV2::Frame adapted{};ProtocolV2::Packet packet{};
    assert(!NoAckTransfer::decode(wrong.data(),wrong.size(),packet,adapted));
    Source broken;broken.bytes.assign(1,7);broken.fail_after_inspection=true;
    NoAckTransfer::Sender sender(&broken,{Source::reset,Source::read});assert(sender.begin(3,"x.bin"));
    assert(!sender.noteSent() && sender.error()==ProtocolV2::ErrorCode::SourceRead && !sender.done());
    FakeHal hal;Nrf24 radio(hal);hal.regs[0x01]=0x3F;hal.regs[0x04]=0xFF;
    assert(radio.disableAutoAck() && hal.regs[0x01]==0 && hal.regs[0x04]==0);
    hal.next_tx_success=false;
    const uint8_t byte=7;
    assert(!radio.transmitOnce(&byte,1,1000,false) && hal.tx_trigger_count==1);
    std::cerr<<"No-ACK codec/sender, empty/boundary/duplicate/CRC/loss/source failure and hardware ACK-disable checks passed\n";
}
