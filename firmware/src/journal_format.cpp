#include "journal_format.h"
#include <string.h>

namespace orun_tlp::journal_format {
void put16(uint8_t* p, uint16_t v) { p[1] = v; p[0] = v >> 8; }
void put32(uint8_t* p, uint32_t v) { for (int i = 3; i >= 0; --i) { p[i] = v; v >>= 8; } }
void put64(uint8_t* p, uint64_t v) { for (int i = 7; i >= 0; --i) { p[i] = v; v >>= 8; } }
uint16_t get16(const uint8_t* p) { return (uint16_t(p[0]) << 8) | p[1]; }
uint32_t get32(const uint8_t* p) { uint32_t v=0; for(unsigned i=0;i<4;++i)v=(v<<8)|p[i]; return v; }
uint64_t get64(const uint8_t* p) { uint64_t v=0; for(unsigned i=0;i<8;++i)v=(v<<8)|p[i]; return v; }
bool erased(const uint8_t* p,size_t n) { for(size_t i=0;i<n;++i) if(p[i]!=0xFF)return false; return true; }
uint32_t crc32(const uint8_t* p,size_t n) { uint32_t c=UINT32_MAX; for(size_t i=0;i<n;++i){c^=p[i];for(unsigned b=0;b<8;++b)c=(c>>1)^((c&1)?0xEDB88320UL:0);}return c^UINT32_MAX; }
bool looksLikeJournalHeader(const uint8_t* p) { const uint8_t m[4]={'O','R','J','4'}; bool marked=false; for(unsigned i=0;i<4;++i){if(p[i]!=0xFF&&p[i]!=m[i])return false;marked|=p[i]!=0xFF;}return marked; }
bool validPacket(const uint8_t* p,uint64_t id,uint64_t device) { tlp::PositionPacket d{}; return id&&tlp::deserializePositionPacket(p,tlp::kPositionPacketSize,&d)&&d.source_device_id==device&&d.sequence_number==uint32_t(id-1)&&(d.flags&tlp::kPositionFlagValidFix)&&((d.flags&tlp::kPositionFlagValidUtcTime)||d.gnss_utc_epoch_seconds==0); }
static void seal(uint8_t* p,unsigned crc,unsigned commit){put32(p+crc,crc32(p,crc));put32(p+commit,kCommit);}
static bool sealed(const uint8_t* p,unsigned crc,unsigned commit){return get32(p+commit)==kCommit&&get32(p+crc)==crc32(p,crc);}
void encodePage(uint64_t gen,uint64_t dev,uint8_t* p){memset(p,0,kStaticHeaderSize);put32(p,kPageMagic);p[4]=kVersion;put64(p+8,gen);put64(p+16,dev);seal(p,56,60);}
bool decodePage(const uint8_t* p,uint64_t dev,uint64_t& gen){if(get32(p)!=kPageMagic||p[4]!=kVersion||p[5]||p[6]||p[7]||get64(p+16)!=dev||!(gen=get64(p+8))||!sealed(p,56,60))return false;for(unsigned i=24;i<56;++i)if(p[i])return false;return true;}
void encodeSequenceEnd(uint64_t end,uint8_t* p){memset(p,0,kSequenceSlotSize);put64(p,end);seal(p,8,12);}
bool decodeSequenceEnd(const uint8_t* p,uint64_t& end){return sealed(p,8,12)&&(end=get64(p))&&!(end%storage_config::kSequenceBlockSize);}
void encodeState(const State& s,uint8_t* p){memset(p,0,kStateSlotSize);put64(p,s.generation);put64(p+8,s.delivered_through);put64(p+16,s.replay_cursor);seal(p,24,28);}
bool decodeState(const uint8_t* p,State& s){if(!sealed(p,24,28)||!(s.generation=get64(p)))return false;s.delivered_through=get64(p+8);s.replay_cursor=get64(p+16);return true;}
void encodeRecord(const Record& r,uint8_t* p){tlp::PositionPacket d{};(void)tlp::deserializePositionPacket(r.packet,sizeof(r.packet),&d);memset(p,0,storage_config::kRecordSize);put32(p,d.sequence_number);put32(p+4,d.gnss_utc_epoch_seconds);put32(p+8,uint32_t(d.latitude_e7));put32(p+12,uint32_t(d.longitude_e7));put32(p+16,uint32_t(d.altitude_mm));put16(p+20,d.hdop_x100);p[22]=d.satellites;p[23]=d.flags;put32(p+24,uint32_t((r.identity-1)>>32));seal(p,28,32);}
bool decodeRecord(const uint8_t* p,uint64_t dev,Record& r){if(!sealed(p,28,32))return false;tlp::PositionPacket d{dev,get32(p),get32(p+4),int32_t(get32(p+8)),int32_t(get32(p+12)),int32_t(get32(p+16)),get16(p+20),p[22],p[23]};const uint64_t ticket=(uint64_t(get32(p+24))<<32)|d.sequence_number;if(ticket==UINT64_MAX)return false;r.identity=ticket+1;return tlp::serializePositionPacket(d,r.packet,sizeof(r.packet))&&validPacket(r.packet,r.identity,dev);}
}  // namespace orun_tlp::journal_format
