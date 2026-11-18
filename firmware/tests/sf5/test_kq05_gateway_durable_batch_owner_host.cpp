// PC-only actual ProductCustodyStore -> 1..5 scope-authorized HMAC issue.
// Existing OCQ2 durable readback only. No device, RF, history erase or release.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <array>

#include "custody_ack_primitives.h"
#include "kq05_gateway_batch_custody_crypto.h"
#include "kq05_gateway_durable_batch_owner.h"

namespace orun_tlp {
struct Kq05BatchCryptoHostPeer {
  static bool verify(const uint8_t* wire, size_t size,
                     const GatewayCustodyAckScope& scope,
                     const uint8_t (&key)[32],
                     const Kq05BatchExactView* objects, uint8_t count) {
    return Kq05GatewayBatchCustodyCrypto::verifyAgainstExactRetained(
        wire,size,scope,key,objects,count);
  }
};
} // namespace orun_tlp

namespace {
using namespace orun_tlp;
namespace ack=kq05_gateway_batch_ack;
namespace fmt=custody_store_product_format;
using BatchResult=Kq05GatewayDurableBatchOwner::Result;
constexpr uint64_t kGateway=UINT64_C(0x2300230023002301);
constexpr uint64_t kTracker=UINT64_C(0x0102030405060708);
constexpr uint32_t kEpoch=7U;

struct FakeNor final : FlashBackend {
  std::array<uint8_t,2U*fmt::kPageSize> bytes{};
  size_t writes=0U;
  bool fail_next=false;

  FakeNor() { bytes.fill(0xFFU); }
  bool begin() override { return true; }
  bool read(uint32_t off,void* dest,size_t n) const override {
    if (!dest || off>bytes.size() || n>bytes.size()-off) return false;
    memcpy(dest,bytes.data()+off,n);
    return true;
  }
  FlashOpResult program(uint32_t off,const void* source,size_t n) override {
    if (source==nullptr || n==0U || off%4U || n%4U ||
        off>bytes.size() || n>bytes.size()-off || n>256U)
      return FlashOpResult::kFailed;
    if (fail_next) { fail_next=false;return FlashOpResult::kFailed; }
    const uint8_t* src=static_cast<const uint8_t*>(source);
    for (size_t i=0U;i<n;++i)
      if ((bytes[off+i]&src[i])!=src[i]) return FlashOpResult::kFailed;
    for (size_t i=0U;i<n;++i) bytes[off+i]&=src[i];
    ++writes;
    return FlashOpResult::kDone;
  }
  FlashOpResult erasePage(uint32_t) override {
    // A Gateway batch custody ACK is never allowed to erase stored data.
    return FlashOpResult::kFailed;
  }
  FlashOpResult pollPending() override { return FlashOpResult::kFailed; }
};

void put64(uint8_t* p,uint64_t v) {
  for (unsigned i=0U;i<8U;++i)
    p[i]=static_cast<uint8_t>(v>>(56U-8U*i));
}
void put32(uint8_t* p,uint32_t v) {
  for (unsigned i=0U;i<4U;++i)
    p[i]=static_cast<uint8_t>(v>>(24U-8U*i));
}
void frame(uint8_t (&out)[100],size_t n,uint32_t seq,
           uint64_t tracker=kTracker) {
  memset(out,0U,sizeof(out));
  assert(n==100U || n==84U);
  out[0]=2U;out[1]=4U;out[2]=2U;
  out[3]=n==100U?1U:2U;
  out[4]=3U;out[5]=1U;out[6]=n==100U?64U:48U;
  put64(out+8U,tracker);put32(out+16U,kEpoch);
  put64(out+20U,42U+seq);
  for (size_t i=28U;i<n-8U;++i)
    out[i]=static_cast<uint8_t>((i*5U+seq*7U)&0xFFU);
  for (size_t i=n-8U;i<n;++i)
    out[i]=static_cast<uint8_t>(0xA0U+i-(n-8U));
}

struct Grants final : GatewayCustodyGrantProvider {
  bool active=true;
  bool revoke_on_second=false;
  bool rotate_key_on_second=false;
  bool rotate_generation_on_second=false;
  uint64_t gateway=kGateway;
  uint64_t tracker=kTracker;
  uint32_t epoch=kEpoch;
  unsigned calls=0U;
  GatewayCustodyAckScope scope() const {
    GatewayCustodyAckScope s{};
    s.tracker_device_id=tracker;s.gateway_device_id=gateway;
    s.key_epoch=epoch;s.custody_floor=12U;s.grant_generation=5U;
    return s;
  }
  static void fixedKey(uint8_t (&out)[32]) {
    for (size_t i=0U;i<32U;++i)
      out[i]=static_cast<uint8_t>(i*7U+11U);
  }
  bool activeGrant(uint64_t tracker_id,uint32_t key_epoch,
                   GatewayCustodyAckScope& out,uint8_t (&key)[32]) override {
    ++calls;
    if (!active || tracker_id!=tracker || key_epoch!=epoch ||
        (revoke_on_second && calls>=2U)) return false;
    out=scope();
    if (rotate_generation_on_second && calls>=2U)
      ++out.grant_generation;
    fixedKey(key);
    if (rotate_key_on_second && calls>=2U)key[0]^=1U;
    return true;
  }
};

struct Fixture {
  FakeNor flash;
  ProductCustodyStore store;
  Grants grants;
  Kq05GatewayDurableBatchOwner issuer;
  ProductCustodyStore::Handle handles[6]{};
  uint8_t contents[6][100]{};
  size_t sizes[6]{};

  Fixture():store(flash,2U),issuer(store,grants,kGateway) {
    assert(store.begin());
  }
  void commit(unsigned seq,uint64_t tracker=kTracker) {
    assert(seq<6U);
    sizes[seq]=seq%2U?84U:100U;
    frame(contents[seq],sizes[seq],seq+1U,tracker);
    assert(store.requestCustody(contents[seq],sizes[seq]) ==
           ProductCustodyStore::Admission::kStarted);
    for (unsigned spins=0U;spins<32U && store.busy();++spins)
      store.poll();
    assert(!store.busy());
    bool durable=false;
    assert(store.takeCustodyResult(durable,handles[seq]));
    assert(durable);
    assert(handles[seq].generation>0U);
  }
  void issueAndVerify(uint8_t n) {
    uint8_t wire[ack::kMaxSize]{};
    size_t len=0U;
    const size_t before=flash.writes;
    assert(issuer.issue(kTracker,kEpoch,handles,n,wire,len)==BatchResult::kReady);
    assert(flash.writes==before);
    assert(len==ack::kHeaderSize + ack::kRefSize*n + ack::kTagSize);
    Kq05BatchExactView views[ack::kMaxObjects]{};
    for (uint8_t i=0U;i<n;++i) {
      views[i].bytes=contents[i];
      views[i].size=sizes[i];
    }
    uint8_t key[32]{};
    Grants::fixedKey(key);
    assert(Kq05BatchCryptoHostPeer::verify(
        wire,len,grants.scope(),key,views,n));
    wire[len-1U]^=1U;
    assert(!Kq05BatchCryptoHostPeer::verify(
        wire,len,grants.scope(),key,views,n));
  }
};

void testNoAckUntilEachFlashCommitAndNoMutation() {
  Fixture f;
  uint8_t one[100]{};
  frame(one,100U,1U);
  uint8_t out[ack::kMaxSize]{};
  memset(out,0xAAU,sizeof(out));
  size_t length=900U;
  assert(f.issuer.issue(kTracker,kEpoch,f.handles,1U,out,length)
         ==BatchResult::kNotDurable);

  assert(f.store.requestCustody(one,100U)==
         ProductCustodyStore::Admission::kStarted);
  assert(f.issuer.issue(kTracker,kEpoch,f.handles,1U,out,length)
         ==BatchResult::kBusy);
  for (unsigned i=0U;i<32U&&f.store.busy();++i)f.store.poll();
  assert(!f.store.busy());
  assert(f.issuer.issue(kTracker,kEpoch,f.handles,1U,out,length)
         ==BatchResult::kBusy); // unconsumed commit outcome
  bool committed=false;
  assert(f.store.takeCustodyResult(committed,f.handles[0]) && committed);
  memcpy(f.contents[0],one,sizeof(one));
  f.sizes[0]=sizeof(one);
  const size_t before=f.flash.writes;
  assert(f.issuer.issue(kTracker,kEpoch,f.handles,1U,out,length)
         ==BatchResult::kReady);
  assert(length==ack::kMinSize);
  assert(f.flash.writes==before);  // sign never writes / erases
  for (unsigned j=1U;j<6U;++j)f.commit(j);
  f.issueAndVerify(5U);
  for (uint8_t n=1U;n<=5U;++n)f.issueAndVerify(n);
}

void testMissingDuplicateForeignAndRevokedFailClosed() {
  Fixture f;
  for (unsigned i=0U;i<6U;++i)
    f.commit(i,i==5U ? kTracker+1U : kTracker);
  const size_t writes=f.flash.writes;
  uint8_t out[ack::kMaxSize]{};
  memset(out,0xA5U,sizeof(out));
  size_t length=700U;
  auto denied=[&](const ProductCustodyStore::Handle* handles,uint8_t n,
                  BatchResult expected) {
    assert(f.issuer.issue(kTracker,kEpoch,handles,n,out,length)==expected);
    for (uint8_t b:out) assert(b==0xA5U);
    assert(length==700U && f.flash.writes==writes);
  };
  denied(nullptr,1U,BatchResult::kDenied);
  denied(f.handles,0U,BatchResult::kDenied);
  denied(f.handles,6U,BatchResult::kDenied);
  ProductCustodyStore::Handle dup[2] = {f.handles[0],f.handles[0]};
  denied(dup,2U,BatchResult::kDenied);
  ProductCustodyStore::Handle stale[2] = {f.handles[0],f.handles[1]};
  ++stale[1].generation;
  denied(stale,2U,BatchResult::kNotDurable);
  denied(f.handles+1U,5U,BatchResult::kDenied); // includes foreign Tracker
  f.grants.active=false;
  denied(f.handles,5U,BatchResult::kDenied);
  f.grants.active=true;
  f.grants.calls=0U;
  f.grants.revoke_on_second=true;
  denied(f.handles,5U,BatchResult::kDenied);
  f.grants.revoke_on_second=false;
  f.grants.calls=0U;
  f.grants.rotate_generation_on_second=true;
  denied(f.handles,5U,BatchResult::kDenied);
  f.grants.rotate_generation_on_second=false;
  f.grants.calls=0U;
  f.grants.rotate_key_on_second=true;
  denied(f.handles,5U,BatchResult::kDenied);
  f.grants.rotate_key_on_second=false;

  // Tamper a confirmed OCQ2 body after commit: CRC mismatch denies ACK.
  const auto h=f.handles[1];
  const size_t off=size_t(h.page)*fmt::kPageSize +
      fmt::kPageHeaderSize + size_t(h.slot)*fmt::kRecordSize + 30U;
  f.flash.bytes[off]^=1U;
  denied(f.handles,5U,BatchResult::kNotDurable);
  f.flash.bytes[off]^=1U;
}

void testRebootReadsOldExactBytesWithoutFreshWrite() {
  Fixture f;
  for (unsigned i=0U;i<5U;++i)f.commit(i);
  const size_t before=f.flash.writes;
  ProductCustodyStore restarted(f.flash,2U);
  assert(restarted.begin() && restarted.ready());
  Grants grants;
  Kq05GatewayDurableBatchOwner recovered(restarted,grants,kGateway);
  uint8_t wire[ack::kMaxSize]{};
  size_t written=0U;
  assert(recovered.issue(kTracker,kEpoch,f.handles,5U,wire,written)
         ==BatchResult::kReady);
  assert(written==88U && f.flash.writes==before);
  // An ACK from Gateway A is not issued by a different local board ID.
  Kq05GatewayDurableBatchOwner impostor(restarted,grants,kGateway+1U);
  assert(impostor.issue(kTracker,kEpoch,f.handles,5U,wire,written)
         ==BatchResult::kDenied);
}

void testFailureDuringFlashCannotMakeAck() {
  Fixture f;
  f.flash.fail_next=true;
  uint8_t object[100]{};
  frame(object,100U,1U);
  assert(f.store.requestCustody(object,100U)==
         ProductCustodyStore::Admission::kStarted);
  for (unsigned i=0U;i<32U && f.store.busy();++i)f.store.poll();
  bool committed=true;
  ProductCustodyStore::Handle h{};
  assert(f.store.takeCustodyResult(committed,h));
  assert(!committed);
  uint8_t out[ack::kMaxSize]{};
  size_t length=0U;
  assert(f.issuer.issue(kTracker,kEpoch,&h,1U,out,length) != BatchResult::kReady);
}

} // namespace

int main() {
  testNoAckUntilEachFlashCommitAndNoMutation();
  testMissingDuplicateForeignAndRevokedFailClosed();
  testRebootReadsOldExactBytesWithoutFreshWrite();
  testFailureDuringFlashCannotMakeAck();
  puts("KQ05 actual OCQ2 1..5 durable readback + grant-recheck Gateway batch HMAC: PASS (HOST ONLY)");
  puts("NO REAL RF, NO TRACKER RELEASE, NO DEVICE FLASH, NO LEGACY ERASE");
}
