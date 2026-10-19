#include "product_custody_store.h"
#include <assert.h>
#include <array>
#include <string.h>
#include <stdio.h>

using namespace orun_tlp;
namespace fmt = orun_tlp::custody_store_product_format;

struct Nor : FlashBackend {
  std::array<uint8_t, fmt::kPageSize*2U> bytes{};
  int writes=0;
  int fail_write=-1;
  bool partial=false;
  bool async=false;
  bool outstanding=false;
  bool unreconciled=false;
  Nor() { bytes.fill(0xFF); }
  bool begin() override { return true; }
  bool read(uint32_t o,void* p,size_t n) const override {
    if(!p || n>bytes.size() || o>bytes.size()-n)return false;
    memcpy(p,bytes.data()+o,n);return true;
  }
  FlashOpResult program(uint32_t o,const void* p,size_t n) override {
    ++writes;
    if(outstanding || !p || (o%4U)||(n%4U)||!n || n>144U ||
       n>bytes.size() || o>bytes.size()-n ||
       n>fmt::kPageSize-(o%fmt::kPageSize))return FlashOpResult::kFailed;
    const uint8_t* src=static_cast<const uint8_t*>(p);
    for(size_t i=0;i<n;++i)if(bytes[o+i]!=0xFFU)return FlashOpResult::kFailed;
    if(writes==fail_write) {
      if(partial)for(size_t i=0;i<n/2U;++i)bytes[o+i]&=src[i];
      return FlashOpResult::kFailed;
    }
    for(size_t i=0;i<n;++i)bytes[o+i]&=src[i];
    if(async){outstanding=true;return FlashOpResult::kPending;}
    return FlashOpResult::kDone;
  }
  FlashOpResult pollPending() override {
    if(!outstanding)return FlashOpResult::kFailed;
    outstanding=false;return FlashOpResult::kDone;
  }
  FlashOpResult erasePage(uint32_t) override { return FlashOpResult::kFailed; }
  bool hasUnreconciledMutation() const override { return unreconciled; }
};

void put64(uint8_t* p,uint64_t v) {
 for(unsigned i=0;i<8;++i)p[i]=static_cast<uint8_t>(v>>(56-8*i));
}
void put32(uint8_t* p,uint32_t v) {
 for(unsigned i=0;i<4;++i)p[i]=static_cast<uint8_t>(v>>(24-8*i));
}
void frame(uint8_t* p,size_t n,uint64_t counter=1U) {
 memset(p,0,n);p[0]=2;p[1]=4;p[2]=2;p[3]=n==100?1:2;
 p[4]=3;p[5]=1;p[6]=n==100?64:48;
 put64(p+8,UINT64_C(0x0102030405060708));put32(p+16,7);put64(p+20,counter);
 for(size_t i=28;i<n;++i)p[i]=static_cast<uint8_t>(i*3+7);
}
using A = ProductCustodyStore::Admission;
void settle(ProductCustodyStore& store) {
 for(int n=0;n<70&&store.busy();++n)store.poll();
 assert(!store.busy());
}

void normalAndReset() {
 Nor nor;
 ProductCustodyStore s(nor,2);
 assert(s.begin());
 uint8_t p[100]{};frame(p,sizeof(p));
 assert(s.requestCustody(p,100)==A::kStarted);
 bool ok=true;ProductCustodyStore::Handle h{};
 assert(!s.takeCustodyResult(ok,h));
 settle(s);
 assert(s.takeCustodyResult(ok,h)&&ok);
 assert(h.page==0&&h.slot==0&&h.generation==1);
 uint8_t read[100]{};size_t n=0;
 assert(s.readConfirmedHeld(h,read,n)==ProductCustodyStore::Lookup::kFound);
 assert(n==100&&memcmp(read,p,n)==0);
 ProductCustodyStore::Handle dup{};
 assert(s.requestCustody(p,100,&dup)==A::kDuplicateHeld);
 assert(dup.page==h.page&&dup.slot==h.slot&&dup.generation==h.generation);
 ProductCustodyStore reboot(nor,2);
 assert(reboot.begin());
 assert(reboot.requestCustody(p,100,&dup)==A::kDuplicateHeld);
 for(uint64_t i=2;i<fmt::kRecordsPerPage+2U;++i){
   uint8_t q[84]{};frame(q,sizeof(q),i);
   assert(reboot.requestCustody(q,84)==A::kStarted);
   settle(reboot);assert(reboot.takeCustodyResult(ok,h)&&ok);
 }
 assert(h.page==1&&h.slot==0&&h.generation==2);
 ProductCustodyStore again(nor,2);
 assert(again.begin());
 assert(again.requestCustody(p,100,&dup)==A::kDuplicateHeld);
}
void asyncNoEarlySuccess() {
 Nor nor;nor.async=true;
 ProductCustodyStore s(nor,2);assert(s.begin());
 uint8_t p[84]{};frame(p,84);
 assert(s.requestCustody(p,84)==A::kStarted);
 bool ok=true;ProductCustodyStore::Handle h{};
 for(int i=0;i<9;++i){s.poll();assert(!s.takeCustodyResult(ok,h));}
 settle(s);
 assert(s.takeCustodyResult(ok,h)&&ok);
}
void cuts() {
 for(int cut=1;cut<=5;++cut){
   Nor nor;nor.fail_write=cut;nor.partial=true;
   ProductCustodyStore s(nor,2);assert(s.begin());
   uint8_t p[100]{};frame(p,100);
   assert(s.requestCustody(p,100)==A::kStarted);
   settle(s);
   bool ok=true;ProductCustodyStore::Handle h{};
   assert(s.takeCustodyResult(ok,h)&&!ok&&s.faulted());
   uint8_t o[100]{};size_t n=0;
   assert(s.readConfirmedHeld(h,o,n)==ProductCustodyStore::Lookup::kError);
   ProductCustodyStore reboot(nor,2);
   assert(!reboot.begin());
 }
}
void rebootAtEveryCommitBoundary() {
 for(int cut=1;cut<=5;++cut){
  Nor nor;
  ProductCustodyStore s(nor,2);assert(s.begin());
  uint8_t p[100]{};frame(p,100);
  assert(s.requestCustody(p,100)==A::kStarted);
  for(int i=0;i<cut;++i)s.poll();
  assert(nor.writes==cut);
  ProductCustodyStore reboot(nor,2);
  const bool recovered=reboot.begin();
  if(cut==1 || cut==2 || cut==4) assert(!recovered);
  else if(cut==3) {
   assert(recovered); // complete empty active page, no custody record yet
   assert(reboot.requestCustody(p,100)==A::kStarted);
   settle(reboot);
   bool done=false;ProductCustodyStore::Handle handle{};
   assert(reboot.takeCustodyResult(done,handle) && done);
  } else {
   assert(recovered);
   ProductCustodyStore::Handle handle{};
   assert(reboot.requestCustody(p,100,&handle)==A::kDuplicateHeld);
  }
 }
}
void noEvictionOnFull() {
 Nor nor;ProductCustodyStore s(nor,2);assert(s.begin());
 bool ok=false;ProductCustodyStore::Handle handle{};
 for(uint64_t i=1;i<=2U*fmt::kRecordsPerPage;++i){
   uint8_t p[84]{};frame(p,84,i);
   assert(s.requestCustody(p,84)==A::kStarted);
   settle(s);
   assert(s.takeCustodyResult(ok,handle) && ok);
 }
 uint8_t overflow[100]{};frame(overflow,100,999);
 assert(s.requestCustody(overflow,100)==A::kNoCapacity);
 ProductCustodyStore reboot(nor,2);assert(reboot.begin());
 assert(reboot.requestCustody(overflow,100)==A::kNoCapacity);
 uint8_t first[84]{};frame(first,84,1);
 assert(reboot.requestCustody(first,84,&handle)==A::kDuplicateHeld);
}
void unreconciledNoAdmit() {
 Nor nor;ProductCustodyStore s(nor,2);
 nor.unreconciled=true;assert(!s.begin());
 nor.unreconciled=false;ProductCustodyStore s2(nor,2);assert(s2.begin());
 uint8_t p[100]{};frame(p,100);
 nor.unreconciled=true;
 assert(s2.requestCustody(p,100)==A::kRejected);
}
void invalidAndCorrupt() {
 Nor nor;ProductCustodyStore s(nor,2);assert(s.begin());
 uint8_t p[100]{};frame(p,100);
 p[4]=0;assert(s.requestCustody(p,100)==A::kRejected);p[4]=3;
 assert(s.requestCustody(p,73)==A::kRejected);
 assert(s.requestCustody(nullptr,100)==A::kRejected);
 assert(s.requestCustody(p,100)==A::kStarted);
 settle(s);bool ok=false;ProductCustodyStore::Handle h{};
 assert(s.takeCustodyResult(ok,h)&&ok);
 nor.bytes[fmt::kPageHeaderSize+20]^=0x01;
 ProductCustodyStore reboot(nor,2);
 assert(!reboot.begin());
}
int main() {
 normalAndReset();asyncNoEarlySuccess();cuts();invalidAndCorrupt();noEvictionOnFull();unreconciledNoAdmit();rebootAtEveryCommitBoundary();
 puts("SF5K3P PRODUCT_SECURE real FlashBackend custody store / sync async reboot cuts: PASS");
}
