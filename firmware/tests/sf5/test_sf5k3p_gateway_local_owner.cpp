#include "gateway_local_custody_owner.h"
#include <assert.h>
#include <array>
#include <string.h>
#include <stdio.h>
using namespace orun_tlp;
namespace fmt=orun_tlp::custody_store_product_format;
static int signs=0;
namespace orun_tlp {
// Test-only signer substitute: tests custody admission ordering, not crypto.
bool GatewayCustodyAckSigner::signAfterDurableAcceptance(
    const uint8_t* object,size_t n,const GatewayCustodyAckScope& scope,
    const uint8_t (&key)[32],uint8_t (&ack)[custody_ack::kWireSize]) {
  ++signs;
  if (!object || (n!=84 && n!=100) || !scope.gateway_device_id || !key[0])
    return false;
  memset(ack,0xAC,sizeof(ack));return true;
}
}
struct Nor : FlashBackend {
  std::array<uint8_t,fmt::kPageSize*2U> bytes{};
  bool async=false,pending=false;
  int writes=0,fail=-1;
  Nor(){bytes.fill(0xFF);}
  bool begin() override{return true;}
  bool read(uint32_t o,void* p,size_t n) const override {
    if(!p||n>bytes.size()||o>bytes.size()-n)return false;
    memcpy(p,bytes.data()+o,n);return true;
  }
  FlashOpResult program(uint32_t o,const void* p,size_t n) override {
    if(pending || !p || !n || n>144 || o%4 || n%4 || n>bytes.size() ||
      o>bytes.size()-n)return FlashOpResult::kFailed;
    if(++writes==fail)return FlashOpResult::kFailed;
    const uint8_t* q=static_cast<const uint8_t*>(p);
    for(size_t i=0;i<n;++i){if(bytes[o+i]!=0xFF)return FlashOpResult::kFailed;bytes[o+i]&=q[i];}
    if(async){pending=true;return FlashOpResult::kPending;}
    return FlashOpResult::kDone;
  }
  FlashOpResult erasePage(uint32_t) override{return FlashOpResult::kFailed;}
  FlashOpResult pollPending() override{if(!pending)return FlashOpResult::kFailed;pending=false;return FlashOpResult::kDone;}
};
void put64(uint8_t* p,uint64_t v){for(unsigned i=0;i<8;++i)p[i]=uint8_t(v>>(56-8*i));}
void put32(uint8_t* p,uint32_t v){for(unsigned i=0;i<4;++i)p[i]=uint8_t(v>>(24-8*i));}
void fill(uint8_t* p,size_t size) {
 memset(p,0,size);p[0]=2;p[1]=4;p[2]=2;p[3]=size==100?1:2;
 p[4]=3;p[5]=1;p[6]=size==100?64:48;
 put64(p+8,0x0102030405060708ULL);put32(p+16,7);put64(p+20,42);
 for(size_t i=28;i<size;++i)p[i]=uint8_t(i*7+3);
}
GatewayCustodyAckScope scope(){GatewayCustodyAckScope s{};s.tracker_device_id=0x0102030405060708ULL;s.gateway_device_id=0x2300230023002301ULL;s.key_epoch=7;s.custody_floor=0;s.grant_generation=1;return s;}
void okScenario(bool async) {
  Nor n;n.async=async;ProductCustodyStore store(n,2);assert(store.begin());
  GatewayLocalCustodyOwner owner(store);
  uint8_t p[100]{};fill(p,100);uint8_t key[32]{};key[0]=1;
  signs=0;
  assert(owner.submit(p,100,scope(),key)==GatewayLocalCustodyOwner::Submit::kStarted);
  assert(signs==0&&!owner.ackReady());
  uint8_t ack[56]{};assert(!owner.takeAck(ack));
  for(int i=0;i<50&&owner.waitingFlash();++i){owner.service();if(owner.waitingFlash())assert(signs==0);}
  assert(signs==1&&owner.ackReady());
  assert(owner.takeAck(ack)&&ack[0]==0xAC);
  assert(!owner.takeAck(ack));
  assert(owner.submit(p,100,scope(),key)==GatewayLocalCustodyOwner::Submit::kAckReady);
  assert(signs==2&&owner.takeAck(ack));
}
void failScenario(){
  Nor n;n.fail=5;ProductCustodyStore store(n,2);assert(store.begin());
  GatewayLocalCustodyOwner owner(store);
  uint8_t p[84]{};fill(p,84);uint8_t key[32]{};key[0]=1;signs=0;
  assert(owner.submit(p,84,scope(),key)==GatewayLocalCustodyOwner::Submit::kStarted);
  for(int i=0;i<20&&owner.waitingFlash();++i)owner.service();
  uint8_t ack[56]{};
  assert(signs==0&&!owner.ackReady()&&!owner.takeAck(ack));
}
void corruptionBeforeFinalReadback() {
  Nor n;
  ProductCustodyStore store(n, 2);
  assert(store.begin());
  GatewayLocalCustodyOwner owner(store);
  uint8_t packet[100]{}; fill(packet, sizeof(packet));
  uint8_t key[32]{}; key[0] = 1;
  signs = 0;
  assert(owner.submit(packet,sizeof(packet),scope(),key)==
         GatewayLocalCustodyOwner::Submit::kStarted);
  // Page body+commit+activation then record body+commit: 5 writes.
  // The actual record readback still has not been evaluated.
  for (unsigned i=0;i<5U;++i) owner.service();
  assert(n.writes==5 && owner.waitingFlash() && signs==0);
  n.bytes[fmt::kPageHeaderSize+10U]^=0x01U;
  owner.service();
  uint8_t ack[56]{};
  assert(signs==0 && !owner.ackReady() && !owner.takeAck(ack));
  assert(store.faulted());
}
int main() {okScenario(false);okScenario(true);failScenario();corruptionBeforeFinalReadback();puts("SF5K3P durable Gateway local custody -> test-signer admission ordering: PASS");}
