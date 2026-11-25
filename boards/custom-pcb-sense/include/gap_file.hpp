#pragma once
#include "protocol_v2.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <numeric>
#include <string>

namespace GapFile {
using Symbol = std::array<uint8_t,20>;
constexpr unsigned Width=16, Repairs=8;
inline uint8_t multiply(uint8_t a, uint8_t b) {
    uint8_t result=0;
    while (b) {
        if (b&1) result ^= a;
        a=static_cast<uint8_t>((a<<1) ^ ((a&128)?0x1d:0));
        b>>=1;
    }
    return result;
}
inline uint8_t inverse(uint8_t a) {
    uint8_t result=1;
    for (unsigned e=254;e;e>>=1) {
        if (e&1) result=multiply(result,a);
        a=multiply(a,a);
    }
    return result;
}
inline uint8_t factor(unsigned row,unsigned col) {return inverse((32+row)^col);}
inline void add(uint8_t* target,const uint8_t* source,unsigned len,uint8_t scale) {
    for(unsigned i=0;i<len;++i) target[i]^=multiply(scale,source[i]);
}
struct Position {bool repair; uint32_t index;};
inline Position position(uint32_t count,uint32_t slot,uint32_t pass) {
    const uint32_t groups=(count+15)/16, total=count+groups*8;
    uint32_t stride=17;
    while(std::gcd(stride,total)!=1) ++stride;
    slot=(slot+total-static_cast<uint32_t>((static_cast<uint64_t>(pass)*stride)%total))%total;
    if(slot>=count){slot-=count;return {true,(slot%groups)*8+slot/groups};}
    const uint32_t fullRows=(count-(groups-1)*16);
    if(slot<fullRows*groups) return {false,(slot%groups)*16+slot/groups};
    slot-=fullRows*groups;
    return {false,(slot%(groups-1))*16+fullRows+slot/(groups-1)};
}
inline bool seek(FILE* f,uint32_t offset){return std::fseek(f,static_cast<long>(offset),SEEK_SET)==0;}
inline bool fill(FILE* f,uint32_t size) {
    uint8_t zeros[256]{};
    while(size) {
        size_t n=std::min<size_t>(size,sizeof zeros);
        if(std::fwrite(zeros,1,n,f)!=n) return false;
        size-=n;
    }
    return std::fflush(f)==0;
}
inline bool payload(FILE* file,uint32_t size,const Position& pos,ProtocolV2::Packet& packet) {
    packet.type=ProtocolV2::PacketType::Data;
    packet.sequence=static_cast<uint16_t>(pos.index);
    const uint32_t offset=pos.repair?(pos.index/8)*320:pos.index*20;
    if(offset>=size || !seek(file,offset)) return false;
    if(!pos.repair) {
        packet.payload_length=std::min<uint32_t>(20,size-offset);
        return std::fread(packet.payload.data(),1,packet.payload_length,file)==packet.payload_length;
    }
    uint8_t block[320]{};
    unsigned n=std::min<uint32_t>(sizeof block,size-offset);
    if(std::fread(block,1,n,file)!=n) return false;
    packet.payload_length=20;
    packet.payload.fill(0);
    for(unsigned col=0;col<(n+19)/20;++col)
        add(packet.payload.data(),block+col*20,20,factor(pos.index%8,col));
    return true;
}

class Store {
    // Three bytes of bookkeeping per block; data and repair payloads live on disk.
    struct Entry {uint8_t lo,hi,repairs;};
    struct Free {void operator()(Entry* p) const {std::free(p);}};
    std::unique_ptr<Entry,Free> map_;
    FILE *data_=nullptr,*repair_=nullptr;
    std::string repairName_;
    uint32_t total_=0, count_=0, bytes_=0, packets_=0;
    // Keep the equation matrix off the RX task's small stack.
    uint8_t matrix_[8][28]{};
    static bool has(const Entry& e,unsigned col) {
        return ((col<8?e.lo:e.hi) & (1u<<(col%8)))!=0;
    }
    unsigned length(uint32_t index) const {return std::min<uint32_t>(20,total_-20*index);}
    bool put(uint32_t index,const uint8_t* src) {
        unsigned n=length(index);
        if(!seek(data_,20*index)||std::fwrite(src,1,n,data_)!=n) return false;
        auto& e=map_.get()[index/16];
        (index%16<8?e.lo:e.hi)|=1u<<(index%8);
        bytes_+=n; ++packets_; return true;
    }
    bool solve(uint32_t group) {
        auto& e=map_.get()[group];
        unsigned k=std::min<uint32_t>(16,count_-group*16), missing=0, cols[16]{};
        for(unsigned i=0;i<k;++i) if(!has(e,i)) cols[missing++]=i;
        if(!missing||missing>static_cast<unsigned>(__builtin_popcount(e.repairs))) return true;
        unsigned eq=0;
        for(unsigned row=0;row<8 && eq<missing;++row) {
            if(!(e.repairs&(1u<<row))) continue;
            auto* a=matrix_[eq++];
            for(unsigned j=0;j<missing;++j) a[j]=factor(row,cols[j]);
            if(!seek(repair_,20*(group*8+row))||std::fread(a+missing,1,20,repair_)!=20) return false;
            for(unsigned j=0;j<k;++j) if(has(e,j)) {
                uint32_t idx=group*16+j;
                Symbol s{}; unsigned n=length(idx);
                if(!seek(data_,idx*20)||std::fread(s.data(),1,n,data_)!=n) return false;
                add(a+missing,s.data(),20,factor(row,j));
            }
        }
        for(unsigned j=0;j<missing;++j) {
            unsigned pivot=j;
            while(pivot<missing && !matrix_[pivot][j]) ++pivot;
            if(pivot==missing) return false;
            if(pivot!=j) std::swap(matrix_[j],matrix_[pivot]);
            auto* a=matrix_[j]; uint8_t norm=inverse(a[j]);
            for(unsigned i=0;i<missing+20;++i) a[i]=multiply(a[i],norm);
            for(unsigned i=0;i<missing;++i) if(i!=j)
                add(matrix_[i],a,missing+20,matrix_[i][j]);
        }
        for(unsigned j=0;j<missing;++j)
            if(!put(group*16+cols[j],matrix_[j]+missing)) return false;
        return true;
    }
public:
    void reset() {
        if(repair_){std::fclose(repair_);repair_=nullptr; std::remove(repairName_.c_str());}
        map_.reset(); data_=nullptr; packets_=bytes_=0;
    }
    bool begin(FILE* data,const std::string& internalPath,uint32_t total,uint32_t count) {
        reset(); data_=data; repairName_=internalPath;total_=total;count_=count;
        if(count) map_.reset(static_cast<Entry*>(std::calloc((count+15)/16,sizeof(Entry))));
        return data_ && (!count||map_) && fill(data_,total);
    }
    bool accept(const ProtocolV2::Packet& p,bool parity) {
        uint32_t group=p.sequence/(parity?8:16);
        if(group>=(count_+15)/16) return true;
        auto& e=map_.get()[group];
        if(parity) {
            unsigned bit=1u<<(p.sequence%8);
            if(p.payload_length!=20 || (e.repairs&bit)) return true;
            unsigned k=std::min<uint32_t>(16,count_-group*16);
            if(__builtin_popcount(e.lo)+__builtin_popcount(e.hi)==static_cast<int>(k)) return true;
            if(!repair_) {
                repair_=std::fopen(repairName_.c_str(),"w+b");
                if(!repair_||!fill(repair_,((count_+15)/16)*160)) return false;
            }
            if(!seek(repair_,20*p.sequence)||std::fwrite(p.payload.data(),1,20,repair_)!=20) return false;
            e.repairs|=bit;
        } else {
            if(p.sequence>=count_ || length(p.sequence)!=p.payload_length || has(e,p.sequence%16)) return true;
            if(!put(p.sequence,p.payload.data())) return false;
        }
        return solve(group);
    }
    bool checksum(ProtocolV2::Crc32& crc) {
        if(!seek(data_,0)) return false;
        crc.reset(); uint8_t buf[256];
        for(uint32_t left=total_;left;) {
            unsigned n=std::min<uint32_t>(left,sizeof buf);
            if(std::fread(buf,1,n,data_)!=n) return false;
            crc.update(buf,n); left-=n;
        }
        return std::fgetc(data_)==EOF && !std::ferror(data_);
    }
    uint32_t bytes() const{return bytes_;}
    uint32_t packets() const{return packets_;}
};
}
