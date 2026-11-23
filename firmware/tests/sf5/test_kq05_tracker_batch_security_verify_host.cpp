// KQ05 Tracker actual SecurityStore V3 root-derived Gateway batch HMAC.
// Host PC only: NO Tracker release, RF RX/TX, device upload or NOR migration.
// Reuses real SF5K3M SecurityStore and ProductRecordStore durable fixtures.
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "kq05_tracker_batch_ack_verifier.h"
#include "kq05_gateway_batch_custody_crypto.h"

#define main sf5k3m_parent_main
#include "test_sf5k3m_real_custody_release.cpp"
#undef main

namespace orun_tlp {
struct Kq05BatchCryptoHostPeer {
  static bool sign(const GatewayCustodyAckScope& scope,
                   const uint8_t (&key)[32],
                   const Kq05BatchExactView* objects, uint8_t count,
                   uint8_t (&wire)[kq05_gateway_batch_ack::kMaxSize],
                   size_t& length) {
    return Kq05GatewayBatchCustodyCrypto::signAfterDurableReadback(
        scope,key,objects,count,wire,length);
  }
};
struct Kq05TrackerBatchVerifyHostPeer {
  static uint8_t count(const AuthenticatedKq05TrackerBatchAck& p) {
    return p.count_;
  }
  static bool authenticated(const AuthenticatedKq05TrackerBatchAck& p) {
    return p.verified_;
  }
  static uint32_t sequence(const AuthenticatedKq05TrackerBatchAck& p,
                           uint8_t index) {
    assert(index < p.count_);
    return p.identities_[index].sequence;
  }
  static void seed(AuthenticatedKq05TrackerBatchAck& p) {
    p.count_=7U;
    p.identities_[0].sequence=999U;
    p.verified_=false;
  }
  static bool untouched(const AuthenticatedKq05TrackerBatchAck& p) {
    return p.count_==7U && p.identities_[0].sequence==999U &&
           !p.verified_;
  }
};
} // namespace orun_tlp

namespace {
namespace ack=orun_tlp::kq05_gateway_batch_ack;
using orun_tlp::Kq05TrackerBatchAckVerifier;
using orun_tlp::Kq05TrackerBatchVerifyResult;
using orun_tlp::AuthenticatedKq05TrackerBatchAck;
using orun_tlp::Kq05BatchExactView;
using orun_tlp::GatewayCustodyAckScope;
using Peer=orun_tlp::Kq05BatchCryptoHostPeer;
using ProofPeer=orun_tlp::Kq05TrackerBatchVerifyHostPeer;

constexpr char kIndependentScopedKey[] =
  "b3b50abe4a260946f7a1e13361e742504736f11df4dfd3b1852e5ae242d5b04f";

void fromHex32(uint8_t (&out)[32]) {
  assert(strlen(kIndependentScopedKey)==64U);
  auto digit=[](char c)->uint8_t {
    if(c>='0' && c<='9')return static_cast<uint8_t>(c-'0');
    if(c>='a' && c<='f')return static_cast<uint8_t>(c-'a'+10);
    assert(false);return 0U;
  };
  for (size_t i=0U;i<32U;++i)
    out[i]=static_cast<uint8_t>((digit(kIndependentScopedKey[2U*i])<<4U) |
                                digit(kIndependentScopedKey[2U*i+1U]));
}

GatewayCustodyAckScope allowedScope() {
  GatewayCustodyAckScope s{};
  s.tracker_device_id=kDevice;
  s.gateway_device_id=kGateA;
  s.key_epoch=7U;
  s.custody_floor=0U;
  s.grant_generation=1U;
  return s;
}

void buildAck(const osc::ExactObject* records,uint8_t n,
              uint8_t (&wire)[ack::kMaxSize],size_t& length) {
  Kq05BatchExactView views[ack::kMaxObjects]{};
  for(uint8_t i=0U;i<n;++i) {
    views[i].bytes=records[i].object;
    views[i].size=records[i].object_size;
  }
  uint8_t key[32]{};
  fromHex32(key);
  assert(Peer::sign(allowedScope(),key,views,n,wire,length));
  memset(key,0,sizeof(key));
}

struct Fixture {
  Flash security_flash;
  SecurityStore security;
  ProductFlash product_flash;
  Source source;
  ProductRecordStore records;
  osc::ExactObject objects[5]{};
  uint8_t cached_count=0U;
  Kq05TrackerBatchAckVerifier verifier;

  explicit Fixture(unsigned initial_records = 5):security(security_flash,security_flash),
            records(product_flash,kProductPages,&source),
            verifier(security) {
    provisionV3(security_flash,security);
    records.begin(kDevice,product_region::Jobs{false,true});
    assert(records.mounted());
    assert(initial_records <= 5U);
    for(uint8_t i=0U;i<initial_records;++i) {
      const osf::RecordIdentity id=append(records);
      objects[i]=makeObject(id);
      // A different valid security-counter makes each exact digest unique
      // without changing any authenticated grant or ciphertext format.
      objects[i].object[27U]=static_cast<uint8_t>(42U+i);
    }
  }
  void admitGrant() {
    assert(Seam::grant(security,0U,kGateA));
    settle(security);
    bool accepted=false;
    assert(Seam::take(security,accepted) && accepted);
  }
  void cacheUpTo(uint8_t count) {
    assert(count<=5U && count>=cached_count);
    for(uint8_t i=cached_count;i<count;++i) cache(records,objects[i]);
    cached_count=count;
  }
};

void testActualRootDerivedFiveAndSubsets() {
  Fixture f;
  uint8_t ack_wire[ack::kMaxSize]{};
  size_t len=0U;
  buildAck(f.objects,5U,ack_wire,len);
  assert(len==88U);
  AuthenticatedKq05TrackerBatchAck proof{};
  ProofPeer::seed(proof);

  assert(f.verifier.verifyFromStore(ack_wire,len,f.records,proof)==
         Kq05TrackerBatchVerifyResult::kNoDurableGrant);
  assert(ProofPeer::untouched(proof));

  f.admitGrant();
  assert(f.verifier.verifyFromStore(ack_wire,len,f.records,proof)!=
         Kq05TrackerBatchVerifyResult::kAuthenticated);
  assert(ProofPeer::untouched(proof));

  f.cacheUpTo(4U);
  assert(f.verifier.verifyFromStore(ack_wire,len,f.records,proof)==
         Kq05TrackerBatchVerifyResult::kNoUniqueObject);
  assert(ProofPeer::untouched(proof));

  f.cacheUpTo(5U);
  ObservationStore::Record before[5]{};
  for(uint8_t i=0U;i<5U;++i) {
    assert(f.records.store().lookup(f.objects[i].identity,before[i])==
           ObservationStore::LookupResult::kFound);
    assert(!before[i].released && !before[i].release_uncertain);
  }
  assert(f.verifier.verifyFromStore(ack_wire,len,f.records,proof)==
         Kq05TrackerBatchVerifyResult::kAuthenticated);
  assert(ProofPeer::authenticated(proof));
  assert(ProofPeer::count(proof)==5U);
  // Authentication by itself must NEVER mutate a release marker.
  for(uint8_t i=0U;i<5U;++i) {
    ObservationStore::Record still{};
    assert(f.records.store().lookup(f.objects[i].identity,still)==
           ObservationStore::LookupResult::kFound);
    assert(!still.released && !still.release_uncertain);
  }
  // The wire is sorted by digest, not sequence. Exactly these five distinct
  // durable identities must be present, without any inferred range ACK.
  bool seen[5]{};
  for(uint8_t i=0U;i<5U;++i) {
    const uint32_t seq=ProofPeer::sequence(proof,i);
    assert(seq>=1U && seq<=5U && !seen[seq-1U]);
    seen[seq-1U]=true;
  }

  // A proper subset must be allowed even when the Tracker has five cached
  // objects: it MUST NOT treat other cached objects as already ACKed.
  for(uint8_t n=1U;n<=5U;++n) {
    buildAck(f.objects,n,ack_wire,len);
    AuthenticatedKq05TrackerBatchAck subset{};
    assert(f.verifier.verifyFromStore(ack_wire,len,f.records,subset)==
           Kq05TrackerBatchVerifyResult::kAuthenticated);
    assert(ProofPeer::count(subset)==n);
  }

  // Real ProductRecordStore cold mount must preserve all five objects and
  // SecurityStore V3 cold mount must preserve the current durable grant.
  ProductRecordStore reboot(f.product_flash,kProductPages,&f.source);
  reboot.begin(kDevice,product_region::Jobs{false,true});
  assert(reboot.mounted());
  SecurityStore sec_reboot(f.security_flash,f.security_flash);
  assert(sec_reboot.begin(DeviceIdentity::fromLegacyUint64(kDevice)));
  settle(sec_reboot);
  Kq05TrackerBatchAckVerifier restarted(sec_reboot);
  buildAck(f.objects,5U,ack_wire,len);
  AuthenticatedKq05TrackerBatchAck after_reboot{};
  assert(restarted.verifyFromStore(ack_wire,len,reboot,after_reboot)==
         Kq05TrackerBatchVerifyResult::kAuthenticated);
}

void testTamperAndRevocationDoNotPublishCapability() {
  Fixture f;
  f.admitGrant();
  f.cacheUpTo(5U);
  uint8_t ack_wire[ack::kMaxSize]{};
  size_t len=0U;
  buildAck(f.objects,5U,ack_wire,len);
  AuthenticatedKq05TrackerBatchAck proof{};
  ProofPeer::seed(proof);

  for(size_t i=0U;i<len;++i) {
    const uint8_t previous=ack_wire[i];
    ack_wire[i]^=0x01U;
    assert(f.verifier.verifyFromStore(ack_wire,len,f.records,proof)!=
           Kq05TrackerBatchVerifyResult::kAuthenticated);
    assert(ProofPeer::untouched(proof));
    ack_wire[i]=previous;
  }
  assert(f.verifier.verifyFromStore(ack_wire,len-1U,f.records,proof)!=
         Kq05TrackerBatchVerifyResult::kAuthenticated);
  assert(ProofPeer::untouched(proof));
  ack_wire[1U]=0x09U; // Backend receipt not Gateway custody ACK.
  assert(f.verifier.verifyFromStore(ack_wire,len,f.records,proof)==
         Kq05TrackerBatchVerifyResult::kInvalid);
  ack_wire[1U]=0x05U; // Old single-object Gateway ACK not batch.
  assert(f.verifier.verifyFromStore(ack_wire,len,f.records,proof)==
         Kq05TrackerBatchVerifyResult::kInvalid);
  ack_wire[1U]=0x0AU;
  assert(ProofPeer::untouched(proof));

  // Different even well-formed object -> same 8B hint must not become
  // authority: the underlying exact digest is bound to full HMAC.
  f.objects[0].object[40U]^=1U;
  buildAck(f.objects,5U,ack_wire,len);
  assert(f.verifier.verifyFromStore(ack_wire,len,f.records,proof)!=
         Kq05TrackerBatchVerifyResult::kAuthenticated);
  f.objects[0].object[40U]^=1U;
  buildAck(f.objects,5U,ack_wire,len);

  assert(Seam::revoke(f.security,0U,kGateA));
  settle(f.security);
  bool revoked=false;
  assert(Seam::take(f.security,revoked) && revoked);
  assert(f.verifier.verifyFromStore(ack_wire,len,f.records,proof)==
         Kq05TrackerBatchVerifyResult::kNoDurableGrant);
  assert(ProofPeer::untouched(proof));
}

} // namespace

#ifndef ORUN_KQ05_TRACKER_BATCH_RELEASE_HOST_TEST
int main() {
  testActualRootDerivedFiveAndSubsets();
  testTamperAndRevocationDoNotPublishCapability();
  puts("KQ05 Tracker REAL SecurityStore grant + full five-record Gateway batch MAC: PASS (HOST ONLY)");
  puts("NO TRACKER RELEASE, NO LORA RX/TX, NO DEVICE FLASH, NO LEGACY ERASE");
}
#endif  // ORUN_KQ05_TRACKER_BATCH_RELEASE_HOST_TEST
