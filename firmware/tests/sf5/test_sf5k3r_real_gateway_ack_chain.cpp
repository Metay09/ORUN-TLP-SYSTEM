// SF5K3R: real Gateway HMAC signer and durable custody owner, linked together.
// Production signer/store/codec/ACK parser; ONLY crypto uses test-only OpenSSL.
// The caller supplies a fixed test key: enrollment and RF remain unimplemented.
#include "gateway_local_custody_owner.h"
#include <assert.h>
#include <array>
#include <stdio.h>
#include <string.h>
using namespace orun_tlp;
namespace fmt = orun_tlp::custody_store_product_format;

struct Nor : FlashBackend {
  std::array<uint8_t, fmt::kPageSize * 2U> bytes{};
  bool async = false, pending = false;
  int writes = 0, fail = -1;
  Nor() { bytes.fill(0xFFU); }
  bool begin() override { return true; }
  bool read(uint32_t o, void* dst, size_t n) const override {
    if (!dst || o > bytes.size() || n > bytes.size()-o) return false;
    memcpy(dst, bytes.data()+o, n);
    return true;
  }
  FlashOpResult program(uint32_t o, const void* src, size_t n) override {
    if (pending || !src || !n || n>144 || o%4 || n%4 ||
        o>bytes.size() || n>bytes.size()-o)
      return FlashOpResult::kFailed;
    if (++writes==fail) return FlashOpResult::kFailed;
    const uint8_t* p=static_cast<const uint8_t*>(src);
    for (size_t i=0;i<n;i++)
      if (bytes[o+i]!=0xFFU) return FlashOpResult::kFailed;
    for (size_t i=0;i<n;i++) bytes[o+i]&=p[i];
    if (async) { pending=true; return FlashOpResult::kPending; }
    return FlashOpResult::kDone;
  }
  FlashOpResult erasePage(uint32_t) override { return FlashOpResult::kFailed; }
  FlashOpResult pollPending() override {
    if (!pending) return FlashOpResult::kFailed;
    pending=false; return FlashOpResult::kDone;
  }
};
uint8_t nibble(char c) {
  if (c>='0' && c<='9') return static_cast<uint8_t>(c-'0');
  if (c>='a' && c<='f') return static_cast<uint8_t>(c-'a'+10);
  assert(false); return 0U;
}
template <size_t N> void fromHex(const char* s, uint8_t (&dst)[N]) {
  assert(strlen(s)==2U*N);
  for(size_t i=0;i<N;i++)
    dst[i]=static_cast<uint8_t>((nibble(s[2U*i])<<4U)|nibble(s[2U*i+1U]));
}
void put64(uint8_t* p,uint64_t v) {
  for (unsigned i=0;i<8U;i++) p[i]=static_cast<uint8_t>(v>>(56U-8U*i));
}
void put32(uint8_t* p,uint32_t v) {
  for (unsigned i=0;i<4U;i++) p[i]=static_cast<uint8_t>(v>>(24U-8U*i));
}
GatewayCustodyAckScope scope() {
  GatewayCustodyAckScope s{};
  s.tracker_device_id=UINT64_C(0x0102030405060708);
  s.gateway_device_id=UINT64_C(0x2300230023002301);
  s.key_epoch=7U; s.custody_floor=0U; s.grant_generation=1U;
  return s;
}
void frame(uint8_t* p,size_t n) {
  memset(p,0,n);
  p[0]=2;p[1]=4;p[2]=2;p[3]=(n==100?1:2);
  p[4]=3;p[5]=1;p[6]=(n==100?64:48);
  put64(p+8,UINT64_C(0x0102030405060708));
  put32(p+16,7U);put64(p+20,42U);
  for (size_t i=0;i<n-36U;i++) p[28U+i]=static_cast<uint8_t>(i*5U+7U);
  for (size_t i=0;i<8U;i++) p[n-8U+i]=static_cast<uint8_t>(0xA0U+i);
}
constexpr char kKey[] =
  "b3b50abe4a260946f7a1e13361e742504736f11df4dfd3b1852e5ae242d5b04f";
constexpr char kPeriodicAck[] =
  "02050164010203040506070823002300230023010000000700000000"
  "0000000192453945bd2224e144572044decab3402105011f5844a453";
constexpr char kEventAck[] =
  "02050154010203040506070823002300230023010000000700000000"
  "00000001bf62c359077620ad82210adadc9b12c72f6467542172aaa8";

void accepted(size_t n,bool async) {
  Nor flash; flash.async=async;
  ProductCustodyStore store(flash,2U);
  assert(store.begin());
  GatewayLocalCustodyOwner owner(store);
  uint8_t object[100]{};frame(object,n);
  uint8_t key[32]{};fromHex(kKey,key);
  uint8_t got[56]{};uint8_t expected[56]{};
  fromHex(n==100U?kPeriodicAck:kEventAck,expected);
  assert(owner.submit(object,n,scope(),key)==
         GatewayLocalCustodyOwner::Submit::kStarted);
  assert(!owner.ackReady() && !owner.takeAck(got));
  for(unsigned i=0U;i<60U && owner.waitingFlash();++i) {
    owner.service();
    if (owner.waitingFlash()) assert(!owner.ackReady());
  }
  assert(!owner.waitingFlash() && owner.ackReady());
  assert(owner.takeAck(got) && memcmp(got,expected,sizeof(got))==0);
  assert(!owner.takeAck(got));
  // Exact duplicate is idempotently re-ACKed without another flash write.
  const int writes=flash.writes;
  assert(owner.submit(object,n,scope(),key)==
         GatewayLocalCustodyOwner::Submit::kAckReady);
  assert(owner.takeAck(got) && memcmp(got,expected,sizeof(got))==0);
  assert(flash.writes==writes);
  // Simulated restart: reread confirmed flash, same valid authentication.
  ProductCustodyStore recovered(flash,2U);
  assert(recovered.begin());
  GatewayLocalCustodyOwner afterReset(recovered);
  assert(afterReset.submit(object,n,scope(),key)==
         GatewayLocalCustodyOwner::Submit::kAckReady);
  assert(afterReset.takeAck(got) && memcmp(got,expected,sizeof(got))==0);
  assert(flash.writes==writes);
}
void interrupted() {
  Nor flash; flash.fail=5;
  ProductCustodyStore store(flash,2U);
  assert(store.begin());
  GatewayLocalCustodyOwner owner(store);
  uint8_t object[84]{};frame(object,sizeof(object));
  uint8_t key[32]{};fromHex(kKey,key);
  uint8_t output[56]{};
  assert(owner.submit(object,sizeof(object),scope(),key)==
         GatewayLocalCustodyOwner::Submit::kStarted);
  for(unsigned i=0U;i<40U && owner.waitingFlash();++i) owner.service();
  assert(!owner.ackReady() && !owner.takeAck(output));
}
int main() {
  accepted(100U,false);
  accepted(84U,false);
  accepted(100U,true);
  accepted(84U,true);
  interrupted();
  puts("SF5K3R real HMAC signed durable custody: PASS (host only)");
}
