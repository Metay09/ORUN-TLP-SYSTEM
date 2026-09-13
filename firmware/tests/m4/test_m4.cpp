#include <assert.h>
#include <array>
#include <string.h>
#include <stdio.h>
#include "history_store.h"
#include "position_flow.h"
#include "gnss_manager.h"

using namespace orun_tlp;
using namespace orun_tlp::storage_config;
using namespace orun_tlp::journal_format;
constexpr uint64_t kDevice = 0x123456789ABCDEF0ULL;

class MemoryFlash : public FlashBackend {
 public:
  std::array<uint8_t,kRegionSize> bytes{};
  int program_budget=-1;
  bool tear_erase=false;
  MemoryFlash(){bytes.fill(0xFF);}
  bool begin() override{return true;}
  bool read(uint32_t o,void* d,size_t n) const override {if(o>bytes.size()||n>bytes.size()-o)return false;memcpy(d,bytes.data()+o,n);return true;}
  bool program(uint32_t o,const void* d,size_t n) override {if(o>bytes.size()||n>bytes.size()-o)return false;auto* p=static_cast<const uint8_t*>(d);for(size_t i=0;i<n;++i){if(program_budget==0)return false;assert(bytes[o+i]==0xFF);bytes[o+i]&=p[i];if(program_budget>0)--program_budget;}return true;}
  bool erasePage(uint32_t p) override {if(p>=kPageCount)return false;const size_t n=tear_erase?kPageSize/2:kPageSize;memset(bytes.data()+p*kPageSize,0xFF,n);return !tear_erase;}
};
void settle(HistoryStore& s){for(unsigned i=0;i<80;++i){s.poll();if(!s.busy())return;}assert(false);}
void start(HistoryStore& s){assert(s.begin(kDevice));settle(s);assert(s.ready()&&s.canAppend());}
HistoryStore::Record allocate(HistoryStore& s,int32_t lat=410000000){uint32_t seq;HistoryStore::Record r;assert(s.nextSequence(seq,r.identity));tlp::PositionPacket p{kDevice,seq,0,lat,-290000000,-10,123,8,5};assert(tlp::serializePositionPacket(p,r.packet,sizeof(r.packet)));return r;}
void append(HistoryStore& s,const HistoryStore::Record& r){assert(s.append(r.packet,r.identity));settle(s);bool ok=false;assert(s.takeAppendResult(ok)&&ok);}
void ordered(const HistoryStore& s){HistoryStore::Record r;uint64_t c=0;uint32_t n=0;while(s.readAfter(c,r)){assert(r.identity>c);c=r.identity;++n;}assert(n==s.count());}

void virginAndFirstInitPowerLoss(){
  MemoryFlash virgin;HistoryStore clean(virgin);start(clean);assert(clean.count()==0);
  for(int cut=0;cut<=80;++cut){
    MemoryFlash f;HistoryStore first(f);assert(first.begin(kDevice));
    f.program_budget=cut; for(unsigned i=0;i<8&&first.busy();++i)first.poll();
    f.program_budget=-1;HistoryStore reboot(f);start(reboot);assert(reboot.count()==0);auto r=allocate(reboot);append(reboot,r);
  }
}
void compactSemanticAndCorruption(){
  MemoryFlash f;HistoryStore s(f);start(s);auto a=allocate(s,412345678);append(s,a);HistoryStore::Record recovered;assert(s.newest(recovered));assert(!memcmp(a.packet,recovered.packet,tlp::kPositionPacketSize));
  // The stored form is compact: it reconstructs protocol/type/device, but packet semantics and bytes are identical.
  assert(kRecordSize==36&&kPageHeaderSize==352&&kRecordsPerPage==104&&kCapacity==728);
  f.bytes[kPageHeaderSize+28]^=1;HistoryStore reboot(f);start(reboot);assert(reboot.count()==0&&reboot.diagnostics().recovery_corruptions==1);
}
void tornRecordAndSequenceReservation(){
  MemoryFlash base;HistoryStore s(base);start(s);auto first=allocate(s);append(s,first);
  for(int cut=0;cut<=int(kRecordSize);++cut){MemoryFlash f=base;HistoryStore a(f);start(a);auto next=allocate(a);f.program_budget=cut;assert(a.append(next.packet,next.identity));settle(a);f.program_budget=-1;HistoryStore b(f);start(b);HistoryStore::Record r;assert(b.lookup(first.identity,r));assert(b.lookup(next.identity,r)==(cut==int(kRecordSize)));auto later=allocate(b);assert(later.identity>next.identity);}
  MemoryFlash seq;HistoryStore initial(seq);start(initial);auto old=allocate(initial); // reservation exists, but no record is required
  for(int cut=0;cut<=16;++cut){MemoryFlash f=seq;HistoryStore rebooting(f);assert(rebooting.begin(kDevice));f.program_budget=cut;settle(rebooting);f.program_budget=-1;HistoryStore final(f);start(final);auto next=allocate(final);assert(next.identity>old.identity);}
}
void wrapAndCircularPageRecovery(){
  MemoryFlash f;HistoryStore s(f);start(s);for(unsigned i=0;i<kCapacity+120;++i)append(s,allocate(s));assert(s.count()<=kCapacity&&s.count()>=kCapacity-kRecordsPerPage);ordered(s);HistoryStore reboot(f);start(reboot);ordered(reboot);
  // Interrupt an oldest-page erase: the other six self-describing pages survive.
  while (reboot.count() < kCapacity) append(reboot, allocate(reboot));
  auto pending=allocate(reboot); f.tear_erase=true;
  assert(reboot.append(pending.packet,pending.identity)); settle(reboot); f.tear_erase=false;
  HistoryStore after(f); start(after); assert(after.count()>=6*kRecordsPerPage); ordered(after);
  // Identity's persisted high word keeps ordering across uint32 wire sequence wrap.
  tlp::PositionPacket wp{kDevice,0,0,410000000,-290000000,-10,123,8,5};
  HistoryStore::Record in{0x100000001ULL,{}};assert(tlp::serializePositionPacket(wp,in.packet,34));
  uint8_t encoded[kRecordSize];encodeRecord(in,encoded);HistoryStore::Record out{};assert(decodeRecord(encoded,kDevice,out)&&out.identity==in.identity&&!memcmp(in.packet,out.packet,34));
}
void cursorDelivery(){
  MemoryFlash f;HistoryStore s(f);start(s);auto a=allocate(s);append(s,a);auto b=allocate(s);append(s,b);HistoryStore::Record r;assert(s.saveReplayCursor(a.identity));settle(s);assert(s.getNextBacklog(r)&&r.identity==b.identity);assert(s.markDeliveredThrough(a.identity));settle(s);assert(s.backlogCount()==1);HistoryStore reboot(f);start(reboot);assert(reboot.deliveredThrough()==a.identity&&reboot.replayCursor()==a.identity);}

MemoryFlash* tx_flash=nullptr;bool radio_available=true;unsigned sends=0;
bool RadioManager::begin(SequenceSource& source){sequences_=&source;device_id_=kDevice;return true;}
bool RadioManager::canSend() const{return radio_available;}
bool RadioManager::encodePosition(const GnssFix& f,uint8_t* out,uint64_t& id){uint32_t seq;if(!sequences_->nextSequence(seq,id))return false;tlp::PositionPacket p{kDevice,seq,f.utc_epoch_seconds,f.latitude_e7,f.longitude_e7,f.altitude_mm,f.hdop_x100,f.satellites,f.flags};return tlp::serializePositionPacket(p,out,tlp::kPositionPacketSize);}
bool RadioManager::sendPositionPacket(const uint8_t* bytes){HistoryStore disk(*tx_flash);start(disk);HistoryStore::Record r;assert(disk.newest(r)&&!memcmp(bytes,r.packet,34));++sends;return true;}
void storeFirstNoDelivery(){MemoryFlash f;tx_flash=&f;HistoryStore s(f);start(s);RadioManager radio;radio.begin(s);PositionFlow flow(s,radio);GnssFix fix{0,410000000,290000000,10,100,8,5};assert(flow.acceptFix(fix,0));while(s.busy()){assert(sends==0);s.poll();}flow.update(1);assert(sends==1&&s.backlogCount()==1);for(unsigned i=0;i<10;++i)flow.update(2);assert(sends==1&&s.deliveredThrough()==0);}
int main(){virginAndFirstInitPowerLoss();compactSemanticAndCorruption();tornRecordAndSequenceReservation();wrapAndCircularPageRecovery();cursorDelivery();storeFirstNoDelivery();puts("M4 journal hardening checks: PASS");}
