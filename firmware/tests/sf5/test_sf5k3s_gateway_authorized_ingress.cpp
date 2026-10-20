// Host-only. Actual ProductCustodyStore, local owner, signer, frame codec and
// ACK parser. Fake NOR and TEST OpenSSL crypto; NEVER enroll a live gateway.
#include "gateway_custody_authorized_ingress.h"
#include "gateway_local_custody_owner.h"
#include "tlp_v2_product_secure.h"

#include <assert.h>
#include <array>
#include <stdio.h>
#include <string.h>

using namespace orun_tlp;
namespace fmt = orun_tlp::custody_store_product_format;
constexpr uint64_t kTracker = UINT64_C(0x0102030405060708);
constexpr uint64_t kGateway = UINT64_C(0x2300230023002301);
constexpr char kKey[] = "b3b50abe4a260946f7a1e13361e742504736f11df4dfd3b1852e5ae242d5b04f";
uint8_t nibble(char c);
constexpr char kExpectedPeriodic[] =
 "02050164010203040506070823002300230023010000000700000000"
 "0000000192453945bd2224e144572044decab3402105011f5844a453";
constexpr char kExpectedEvent[] =
 "02050154010203040506070823002300230023010000000700000000"
 "00000001bf62c359077620ad82210adadc9b12c72f6467542172aaa8";

struct Nor : FlashBackend {
  std::array<uint8_t,fmt::kPageSize*2U> data{};
  bool async=false, outstanding=false;
  int writes=0, fail=-1;
  Nor() { data.fill(0xFFU); }
  bool begin() override { return true; }
  bool read(uint32_t o,void* target,size_t n) const override {
    if (!target || o>data.size() || n>data.size()-o) return false;
    memcpy(target,data.data()+o,n);return true;
  }
  FlashOpResult program(uint32_t o,const void* bytes,size_t n) override {
    if(outstanding || !bytes || n==0 || n>144 || o%4 || n%4 ||
       o>data.size() || n>data.size()-o) return FlashOpResult::kFailed;
    if(++writes==fail)return FlashOpResult::kFailed;
    const uint8_t* p=static_cast<const uint8_t*>(bytes);
    for(size_t i=0;i<n;i++)if(data[o+i]!=0xFFU)return FlashOpResult::kFailed;
    for(size_t i=0;i<n;i++)data[o+i]&=p[i];
    if(async){outstanding=true;return FlashOpResult::kPending;}
    return FlashOpResult::kDone;
  }
  FlashOpResult erasePage(uint32_t) override {return FlashOpResult::kFailed;}
  FlashOpResult pollPending() override {
    if(!outstanding)return FlashOpResult::kFailed;
    outstanding=false;return FlashOpResult::kDone;
  }
};

struct Grants final : GatewayCustodyGrantProvider {
  bool enrolled=true;
  bool wrong_tracker=false,wrong_gateway=false,wrong_epoch=false;
  uint32_t generation=1U;
  uint32_t floor=0U;
  unsigned lookups=0;
  bool activeGrant(uint64_t tracker,uint32_t epoch,
                   GatewayCustodyAckScope& scope,uint8_t (&key)[32]) override {
    ++lookups;
    for(unsigned i=0;i<32;i++)
      key[i]=uint8_t((nibble(kKey[2U*i])<<4U)|nibble(kKey[2U*i+1U]));
    if (!enrolled || tracker!=kTracker || epoch!=7U) return false;
    scope.tracker_device_id=wrong_tracker?kTracker+1:kTracker;
    scope.gateway_device_id=wrong_gateway?kGateway+1:kGateway;
    scope.key_epoch=wrong_epoch?8U:7U;
    scope.custody_floor=floor;
    scope.grant_generation=generation;
    return true;
  }
};

void put64(uint8_t* p,uint64_t v) {
  for(unsigned i=0;i<8;i++)p[i]=uint8_t(v>>(56U-8U*i));
}
void put32(uint8_t* p,uint32_t v) {
  for(unsigned i=0;i<4;i++)p[i]=uint8_t(v>>(24U-8U*i));
}
void frame(uint8_t* p,size_t n,bool custody=true) {
  memset(p,0,n);
  p[0]=2;p[1]=4;p[2]=2;p[3]=(n==100?1:2);
  p[4]=uint8_t(custody?3:1);p[5]=1;p[6]=uint8_t(n==100?64:48);
  put64(p+8,kTracker);put32(p+16,7U);put64(p+20,42U);
  for(size_t i=0;i<n-36U;i++)p[28U+i]=uint8_t(i*5U+7U);
  for(size_t i=0;i<8U;i++)p[n-8U+i]=uint8_t(0xA0U+i);
}
uint8_t nibble(char c) {
  if(c>='0'&&c<='9')return uint8_t(c-'0');
  if(c>='a'&&c<='f')return uint8_t(c-'a'+10);
  assert(false);return 0;
}
void checkExpected(const uint8_t (&ack)[56],size_t size) {
  const char* hex=size==100?kExpectedPeriodic:kExpectedEvent;
  assert(strlen(hex)==112U);
  for(size_t i=0;i<56U;i++)
    assert(ack[i]==uint8_t((nibble(hex[2U*i])<<4)|nibble(hex[2U*i+1U])));
}

void accepted(size_t size,bool async) {
  Nor nor;nor.async=async;
  ProductCustodyStore store(nor,2U);assert(store.begin());
  GatewayLocalCustodyOwner owner(store);Grants grant;
  GatewayCustodyAuthorizedIngress ingress(owner,grant,kGateway);
  uint8_t bytes[100]{};frame(bytes,size);
  uint8_t ack[56]{};
  assert(ingress.receive(bytes,size)==GatewayCustodyAuthorizedIngress::Result::kStarted);
  assert(ingress.pending());
  assert(!ingress.takeAuthorizedAck(ack));
  assert(ingress.receive(bytes,size)==GatewayCustodyAuthorizedIngress::Result::kBusy);
  for(unsigned i=0;i<100U&&ingress.pending()&&!owner.ackReady();i++){
    ingress.service();
    if(owner.waitingFlash())assert(!owner.ackReady());
  }
  assert(owner.ackReady());
  assert(ingress.takeAuthorizedAck(ack));checkExpected(ack,size);
  assert(!ingress.pending());
  const int writes=nor.writes;
  assert(ingress.receive(bytes,size)==GatewayCustodyAuthorizedIngress::Result::kAckReady);
  memset(ack,0,sizeof(ack));
  assert(ingress.takeAuthorizedAck(ack));checkExpected(ack,size);
  assert(nor.writes==writes);
}

void deniedBeforeFlash() {
  Nor nor;ProductCustodyStore store(nor,2U);assert(store.begin());
  GatewayLocalCustodyOwner owner(store);Grants grant;
  GatewayCustodyAuthorizedIngress ingress(owner,grant,kGateway);
  uint8_t bytes[100]{};frame(bytes,sizeof(bytes));
  grant.enrolled=false;
  assert(ingress.receive(bytes,sizeof(bytes))==GatewayCustodyAuthorizedIngress::Result::kDenied);
  grant.enrolled=true;grant.wrong_gateway=true;
  assert(ingress.receive(bytes,sizeof(bytes))==GatewayCustodyAuthorizedIngress::Result::kDenied);
  grant.wrong_gateway=false;grant.wrong_tracker=true;
  assert(ingress.receive(bytes,sizeof(bytes))==GatewayCustodyAuthorizedIngress::Result::kDenied);
  grant.wrong_tracker=false;grant.wrong_epoch=true;
  assert(ingress.receive(bytes,sizeof(bytes))==GatewayCustodyAuthorizedIngress::Result::kDenied);
  grant.wrong_epoch=false;grant.generation=0U;
  assert(ingress.receive(bytes,sizeof(bytes))==GatewayCustodyAuthorizedIngress::Result::kDenied);
  grant.generation=1U;grant.floor=UINT32_MAX;
  assert(ingress.receive(bytes,sizeof(bytes))==GatewayCustodyAuthorizedIngress::Result::kDenied);
  grant.floor=0U;
  frame(bytes,sizeof(bytes),false);
  assert(ingress.receive(bytes,sizeof(bytes))==GatewayCustodyAuthorizedIngress::Result::kDenied);
  frame(bytes,sizeof(bytes));
  assert(ingress.receive(bytes,83U)==GatewayCustodyAuthorizedIngress::Result::kDenied);
  bytes[0]=1U;
 assert(ingress.receive(bytes,sizeof(bytes))==GatewayCustodyAuthorizedIngress::Result::kDenied);
  assert(nor.writes==0);
}

void revokedAfterDurability() {
  Nor nor;nor.async=true;
  ProductCustodyStore store(nor,2U);assert(store.begin());
  GatewayLocalCustodyOwner owner(store);Grants grant;
  GatewayCustodyAuthorizedIngress ingress(owner,grant,kGateway);
  uint8_t bytes[84]{};frame(bytes,sizeof(bytes));
  uint8_t out[56];memset(out,0x5A,sizeof(out));
  assert(ingress.receive(bytes,sizeof(bytes))==GatewayCustodyAuthorizedIngress::Result::kStarted);
  for(unsigned i=0;i<100U&&!owner.ackReady();++i)ingress.service();
  assert(owner.ackReady());
  grant.generation=2U; // Authoritative grant changed after durable commit.
  assert(!ingress.takeAuthorizedAck(out));
  for(auto b:out)assert(b==0x5AU);
  assert(!ingress.pending());
}

void failedFlashNoAck() {
  Nor nor;nor.fail=5;
  ProductCustodyStore store(nor,2U);assert(store.begin());
  GatewayLocalCustodyOwner owner(store);Grants grant;
  GatewayCustodyAuthorizedIngress ingress(owner,grant,kGateway);
  uint8_t bytes[84]{};frame(bytes,sizeof(bytes));
  uint8_t out[56]{};
  assert(ingress.receive(bytes,sizeof(bytes))==GatewayCustodyAuthorizedIngress::Result::kStarted);
  for(unsigned i=0;i<100U&&ingress.pending();++i)ingress.service();
  assert(!ingress.pending()&&!owner.ackReady());
  assert(!ingress.takeAuthorizedAck(out));
}

int main() {
  accepted(100U,false);accepted(84U,false);
  accepted(100U,true);accepted(84U,true);
  deniedBeforeFlash();revokedAfterDurability();failedFlashNoAck();
  puts("SF5K3S Gateway scoped admission + flash + revocation: PASS (host only)");
}
