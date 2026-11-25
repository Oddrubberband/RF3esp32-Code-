#pragma once
#include "reliable_transfer_v2.hpp"
#include <cstdio>
#include <memory>
#include <new>
#include <vector>
#include <string>


namespace Delivery {
uint32_t observationId();
// All sequence sets are scoped to one on-air transmission ID. A missing
// allocation makes the observation unavailable without aborting a transfer.
class Bits {
    unsigned limit_;
    std::unique_ptr<uint8_t[]> seen_;
    int count_;
public:
    explicit Bits(unsigned limit = 0) : limit_(limit),
        seen_(limit ? new(std::nothrow) uint8_t[(limit+7)/8]() : nullptr),
        count_(limit && !seen_ ? -1 : 0) {}
    void add(unsigned sequence) {
        if (!seen_ || sequence >= limit_) return;
        auto& byte = seen_[sequence/8];
        const uint8_t mask = uint8_t(1u << (sequence%8));
        if (!(byte & mask)) { byte |= mask; ++count_; }
    }
    int count() const { return count_; }
};

struct Row {
    const char* direction;
    const char* mode;
    uint32_t id, instance;
    int planned, sent=-1, received=-1, first=-1, retries=-1;
    bool verified=false, closed=false;
    const char* result="Observing";
    Row(const char* d="RX", const char* m="ack", uint32_t i=0, int p=-1)
        : direction(d), mode(m), id(i), instance(i ? (observationId() | 1u) : 0), planned(p) {}
    std::string line(const char* board) const {
        char data[220];
        std::snprintf(data,sizeof(data),"DELIVERY1 %s %s %s %08lX %08lX %d %d %d %d %d %d %d %s\r\n",
            board,direction,mode,(unsigned long)id,(unsigned long)instance,planned,sent,
            received,first,retries,int(verified),int(closed),result);
        return data;
    }
};

class Rx {
    Row row_;
    Bits bits_;
    uint64_t last_emit_=0, last_input_=0;
    bool dirty_=false, tagged_=false, final_boundary_=false;
    void start(uint32_t id,int planned,const char* mode,uint64_t now,std::vector<Row>& out) {
        if (row_.id==id && std::string_view(row_.mode)==mode) {
            if (planned>=0) row_.planned=planned;
            return;
        }
        if (row_.id && !row_.closed) { row_.closed=true; out.push_back(row_); }
        row_=Row("RX",mode,id,planned);
        tagged_=false; final_boundary_=false;
        if (std::string_view(mode)=="noack") row_.instance &= ~1u;
        bits_=Bits(planned<0 ? ProtocolV2::kMaxPacketCount : unsigned(planned));
        row_.received=bits_.count(); dirty_=true; last_emit_=last_input_=now;
        out.push_back(row_);
    }
    void publish(std::vector<Row>& out) { out.push_back(row_); dirty_=false; }
public:
    void text(uint32_t id, unsigned plan,unsigned seq,bool complete,uint64_t now,std::vector<Row>& out) {
        start(id,plan,"text",now,out);
        last_input_=now;
        bits_.add(seq); row_.received=bits_.count(); dirty_=true;
        if (row_.received==row_.planned) row_.sent=row_.planned;
        if (complete) { row_.result="Reassembled"; row_.closed=true; publish(out); }
    }
    void file(const ProtocolV2::Packet& p,bool parity,bool noack,ReliableTransferV2::ReceiverEvent event,
              uint64_t now,std::vector<Row>& out) {
        using T=ProtocolV2::PacketType;
        using E=ReliableTransferV2::ReceiverEvent;
        if (p.type!=T::Start && p.type!=T::Data && p.type!=T::End && p.type!=T::Cancel) return;
        if (p.type!=T::Start && row_.id!=p.transfer_id && row_.id && !row_.closed) return;
        const int plan=(p.type==T::Start || p.type==T::End) ? int(p.total_packets) : -1;
        start(p.transfer_id,plan,noack?"noack":"ack",now,out);
        last_input_=now;
        if (final_boundary_) return;
        if (p.type==T::Data && !parity) {
            bits_.add(p.sequence); row_.received=bits_.count(); dirty_=true;
        }
        if (noack && p.type==T::End) row_.sent=row_.planned;
        if (row_.planned>=0 && row_.received==row_.planned) row_.sent=row_.planned;
        if (event==E::Completed && !row_.verified) {
            row_.verified=true; row_.result="Completed"; row_.closed=!noack;
            publish(out);
        } else if (event==E::Cancelled || p.type==T::Cancel) end(p.transfer_id,"Cancelled",out);
        else if (event==E::Failed) end(p.transfer_id,"Failed",out);
    }
    void beginPasses(uint32_t id,int plan,uint32_t observation,uint64_t now,std::vector<Row>& out) {
        if (tagged_ && row_.instance==observation && row_.id==id) return;
        if (row_.id && !row_.closed) { row_.closed=true; publish(out); }
        row_=Row("RX","noack",id,plan); row_.instance=observation;
        bits_=Bits(plan); row_.received=bits_.count(); tagged_=true; final_boundary_=false;
        last_emit_=last_input_=now; publish(out);
    }
    void boundary(uint32_t id,int plan,unsigned flags,uint32_t observation,std::vector<Row>& out) {
        if (id!=row_.id || row_.planned!=plan || (tagged_ && observation!=row_.instance)) return;
        row_.sent=plan;
        if (flags&1) row_.first=row_.received;
        if (flags&2) { final_boundary_=true; row_.closed=true; if (!row_.verified) row_.result="Incomplete"; }
        publish(out);
    }
    void end(uint32_t id,const char* result,std::vector<Row>& out) {
        if (!id || row_.id!=id) return;
        row_.result=result; row_.closed=true; publish(out);
    }
    void poll(uint64_t now,bool text,std::vector<Row>& out) {
        if (!row_.id) return;
        if (!row_.closed && now-last_input_ >= (text ? 2000u : 10000u)) {
            row_.closed=true;
            if (!row_.verified) row_.result="ObservationEnded";
            publish(out);
        } else if (dirty_ && now-last_emit_ >= 1000) {
            publish(out); last_emit_=now;
        }
    }
};
}

