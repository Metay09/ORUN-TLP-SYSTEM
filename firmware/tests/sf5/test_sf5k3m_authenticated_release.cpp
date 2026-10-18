// SF5K3M real SecurityStore + real ProductRecordStore/ObservationStore
// post-HMAC durable release integration. Synthetic ciphertext-shaped object
// remains TEST-ONLY; this does not prove actual Gateway durable custody.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <vector>

#include "custody_ack_release_owner.h"
#include "product_record_store.h"
#include "product_region.h"

// Reuse actual SF5K3L HMAC verifier and actual SF5K3J flash/policy fixture,
// but do not rerun either previous test main.
#define ORUN_SF5K3M_HOST_TEST 1
#include "test_sf5k3l_custody_ack_crypto.cpp"

namespace {
namespace pr=orun_tlp::product_region;
using orun_tlp::CustodyAckReleaseOwner;
using orun_tlp::ProductRecordStore;
using orun_tlp::FlashBackend;
using orun_tlp::FlashOpResult;
using orun_tlp::ObservationStore;
using orun_tlp::ObservationIncarnationSource;
using orun_tlp::PeriodicRecordInputs;
using orun_tlp::AuthenticatedCustodyAck;
using orun_tlp::CustodyAckVerifier;
using orun_tlp::CustodyAckVerifyResult;

constexpr uint16_t kProductPages=6U;

class ProductFlash : public FlashBackend {
 public:
  ProductFlash():bytes(size_t(kProductPages)*4096U,0xFFU){}
  bool begin() override {return true;}
  bool read(uint32_t off,void* dest,size_t n) const override {
    if(fail_next_read) {
      fail_next_read=false;
      return false;
    }
    if(dest==nullptr || uint64_t(off)+n>bytes.size())return false;
    memcpy(dest,bytes.data()+off,n);
    return true;
  }
  FlashOpResult program(uint32_t off,const void* data,size_t n) override {
    if(pending_ || data==nullptr || n==0U || (off&3U) ||
       (n&3U) || uint64_t(off)+n>bytes.size())
      return FlashOpResult::kFailed;
    if(fail_next_program) {
      fail_next_program=false;
      return FlashOpResult::kFailed;
    }
    const uint8_t* input=static_cast<const uint8_t*>(data);
    for(size_t i=0;i<n;++i)
      if((bytes[off+i]&input[i])!=input[i])return FlashOpResult::kFailed;
    for(size_t i=0;i<n;++i)bytes[off+i]&=input[i];
    if(!async)return FlashOpResult::kDone;
    pending_=true;
    return FlashOpResult::kPending;
  }
  FlashOpResult erasePage(uint32_t page) override {
    if(pending_ || page>=kProductPages)return FlashOpResult::kFailed;
    memset(bytes.data()+size_t(page)*4096U,0xFF,4096U);
    if(!async)return FlashOpResult::kDone;
    pending_=true;
    return FlashOpResult::kPending;
  }
  FlashOpResult pollPending() override {
    if(!pending_)return FlashOpResult::kFailed;
    pending_=false;
    return FlashOpResult::kDone;
  }
  bool async=false;
  bool fail_next_program=false;
  mutable bool fail_next_read=false;
  std::vector<uint8_t> bytes;
 private:
  bool pending_=false;
};

class ProductIncarnation final: public ObservationIncarnationSource {
 public:
  bool generate(uint64_t& out) override {
    out=UINT64_C(0x0102030405060708);
    return true;
  }
};

PeriodicRecordInputs report() {
  PeriodicRecordInputs x{};
  x.period_duration_seconds=3600U;
  x.location.valid=true;
  x.location.latitude_e7=376102718;
  x.location.longitude_e7=280545028;
  x.location.hdop_x100=120U;
  x.location.satellites=7U;
  x.location.age_valid=true;
  x.location.age_seconds=3U;
  x.battery_valid=true;
  x.battery_mv=4100U;
  x.battery_state=1U;
  x.health.valid=true;
  return x;
}
void storeOne(ProductRecordStore& product,osc::ExactObject& exact) {
  assert(product.submitPeriodic(report())==ProductRecordStore::Submit::kAccepted);
  ProductRecordStore::Outcome outcome{};
  bool done=false;
  for(unsigned n=0U;n<10000U&&!done;++n) {
    product.poll();
    done=product.takeOutcome(outcome);
  }
  assert(done&&outcome.stored&&outcome.sequence==1U);
  makeObject(exact); // independently frozen SF5K3L MAC oracle bytes
  exact.identity.incarnation=outcome.incarnation;
  exact.identity.sequence=outcome.sequence;
  assert(product.submitExactCache(exact)==
         ProductRecordStore::CacheSubmit::kStarted);
  ProductRecordStore::CacheOutcome cached{};
  done=false;
  for(unsigned n=0U;n<10000U&&!done;++n){
    product.poll();
    done=product.takeCacheOutcome(cached);
  }
  assert(done&&cached.durable);
  assert(product.custodyCandidateMatchesRetainedExact(exact));
}

void testDurableReleaseAndReboot(bool async) {
  Flash sec_flash;
  SecurityStore security(sec_flash,sec_flash);
  provisionV3(sec_flash,security);
  assert(Seam::grant(security,0U,kGateA));
  settle(security);
  bool committed=false;
  assert(Seam::take(security,committed)&&committed);

  ProductFlash product_flash;
  product_flash.async=async;
  ProductIncarnation source;
  pr::Jobs jobs{false,true};
  ProductRecordStore product(product_flash,kProductPages,&source);
  // begin requires a synchronous flash mount, then async device mutations.
  product_flash.async=false;
  product.begin(kDevice,jobs);
  assert(product.mounted());
  product_flash.async=async;
  osc::ExactObject exact{};
  storeOne(product,exact);
  uint8_t ack[56]{};
  assert(hex(kOracleAck,ack,sizeof(ack)));
  CustodyAckVerifier verifier(security);
  AuthenticatedCustodyAck proof{};
  assert(verifier.verifyFromStore(ack,sizeof(ack),product,proof)==
         CustodyAckVerifyResult::kAuthenticated);

  CustodyAckReleaseOwner release(security,product);
  assert(release.submitAuthenticated(AuthenticatedCustodyAck{})==
         CustodyAckReleaseOwner::Submit::kRejected);
  assert(release.submitAuthenticated(proof)==
         CustodyAckReleaseOwner::Submit::kStarted);
  assert(release.submitAuthenticated(proof)==
         CustodyAckReleaseOwner::Submit::kBusy);
  assert(release.service()==CustodyAckReleaseOwner::Service::kWaitingFlash);
  assert(product.submitPeriodic(report())==ProductRecordStore::Submit::kBusy);

  CustodyAckReleaseOwner::Service finish=CustodyAckReleaseOwner::Service::kIdle;
  for(unsigned n=0U;n<1000U;++n){
    product.poll();
    finish=release.service();
    if(finish!=CustodyAckReleaseOwner::Service::kWaitingFlash)break;
  }
  assert(finish==CustodyAckReleaseOwner::Service::kDurablyReleased);
  assert(!release.pending());
  assert(release.service()==CustodyAckReleaseOwner::Service::kIdle);
  ObservationStore::Record rec{};
  assert(product.store().lookup(exact.identity,rec)==
         ObservationStore::LookupResult::kFound);
  assert(rec.released);
  assert(product.diagnostics().custody_release_committed==1U);
  assert(!product.custodyCandidateMatchesRetainedExact(exact));
  uint16_t count=9U;
  osc::ExactObject candidates[4]{};
  assert(product.snapshotCustodyCandidates(candidates,count)&&count==0U);
  assert(verifier.verifyFromStore(ack,sizeof(ack),product,proof)==
         CustodyAckVerifyResult::kNoUniqueObject);
  assert(release.submitAuthenticated(proof)==
         CustodyAckReleaseOwner::Submit::kRejected);

  // Re-mount same byte-for-byte non-erased product flash: the release is
  // permanently authoritative and the old exact object is not outstanding.
  product_flash.async=false;
  ProductRecordStore reboot(product_flash,kProductPages,&source);
  reboot.begin(kDevice,jobs);
  assert(reboot.mounted());
  assert(reboot.store().lookup(exact.identity,rec)==
         ObservationStore::LookupResult::kFound);
  assert(rec.released);
  assert(reboot.snapshotCustodyCandidates(candidates,count)&&count==0U);
  CustodyAckReleaseOwner restarted(security,reboot);
  assert(restarted.submitAuthenticated(proof)==
         CustodyAckReleaseOwner::Submit::kRejected);
}

void testRevocationAndFailureKeepRetained() {
  Flash sec_flash;
  SecurityStore security(sec_flash,sec_flash);
  provisionV3(sec_flash,security);
  assert(Seam::grant(security,0U,kGateA));
  settle(security);
  bool done=false;
  assert(Seam::take(security,done)&&done);

  ProductFlash flash;
  ProductIncarnation source;
  pr::Jobs jobs{false,true};
  ProductRecordStore records(flash,kProductPages,&source);
  records.begin(kDevice,jobs);
  assert(records.mounted());
  osc::ExactObject exact{};
  storeOne(records,exact);
  uint8_t ack[56]{};
  assert(hex(kOracleAck,ack,sizeof(ack)));
  CustodyAckVerifier verifier(security);
  AuthenticatedCustodyAck proof{};
  assert(verifier.verifyFromStore(ack,sizeof(ack),records,proof)==
         CustodyAckVerifyResult::kAuthenticated);
  CustodyAckReleaseOwner release(security,records);

  // Authentication alone cannot skip a durable revocation. Revalidate at
  // release admission, not just at HMAC generation time.
  assert(Seam::revoke(security,0U,kGateA));
  assert(release.submitAuthenticated(proof)==
         CustodyAckReleaseOwner::Submit::kRejected);
  settle(security);
  assert(Seam::take(security,done)&&done);
  assert(release.submitAuthenticated(proof)==
         CustodyAckReleaseOwner::Submit::kRejected);

  // Reenroll under a new generation: previous proof remains stale.
  assert(Seam::grant(security,0U,kGateA));
  settle(security);
  assert(Seam::take(security,done)&&done);
  assert(release.submitAuthenticated(proof)==
         CustodyAckReleaseOwner::Submit::kRejected);

  ObservationStore::Record record{};
  assert(records.store().lookup(exact.identity,record)==
         ObservationStore::LookupResult::kFound);
  assert(!record.released);
}

void testFlashReleaseFailureNeverClaimsDurability() {
  Flash sec_flash;
  SecurityStore security(sec_flash,sec_flash);
  provisionV3(sec_flash,security);
  assert(Seam::grant(security,0U,kGateA));
  settle(security);
  bool committed=false;
  assert(Seam::take(security,committed)&&committed);

  ProductFlash flash;
  ProductIncarnation source;
  pr::Jobs jobs{false,true};
  ProductRecordStore product(flash,kProductPages,&source);
  product.begin(kDevice,jobs);
  assert(product.mounted());
  osc::ExactObject exact{};
  storeOne(product,exact);
  uint8_t ack[56]{};
  assert(hex(kOracleAck,ack,sizeof(ack)));
  CustodyAckVerifier verifier(security);
  AuthenticatedCustodyAck proof{};
  assert(verifier.verifyFromStore(ack,sizeof(ack),product,proof)==
         CustodyAckVerifyResult::kAuthenticated);

  CustodyAckReleaseOwner release(security,product);
  // Physical flash refuses the release-marker write BEFORE mutating.
  // No TX_DONE, HMAC acceptance, or requestRelease admission can
  // substitute for an actual committed/readback release marker.
  flash.fail_next_program=true;
  assert(release.submitAuthenticated(proof)==
         CustodyAckReleaseOwner::Submit::kStarted);
  CustodyAckReleaseOwner::Service finish=
      CustodyAckReleaseOwner::Service::kWaitingFlash;
  for(unsigned n=0U;n<100U;++n) {
    product.poll();
    finish=release.service();
    if(finish!=CustodyAckReleaseOwner::Service::kWaitingFlash)break;
  }
  assert(finish==CustodyAckReleaseOwner::Service::kNotDurable);
  assert(product.diagnostics().custody_release_failed==1U);
  ObservationStore::Record record{};
  assert(product.store().lookup(exact.identity,record)==
         ObservationStore::LookupResult::kFound);
  assert(!record.released);

  // Reboot may now return a retained record or a fail-closed fault, but
  // never silently pretend that this ACK durably released the record.
  ProductRecordStore reboot(flash,kProductPages,&source);
  reboot.begin(kDevice,jobs);
  if(reboot.mounted()) {
    const auto state=reboot.store().lookup(exact.identity,record);
    assert(state!=ObservationStore::LookupResult::kFound ||
           !record.released);
  }
}

} // namespace
int main() {
  testFlashReleaseFailureNeverClaimsDurability();
  testDurableReleaseAndReboot(false);
  testDurableReleaseAndReboot(true);
  testRevocationAndFailureKeepRetained();
  puts("SF5K3M real ProductRecordStore authenticated custody release/reboot: PASS");
  return 0;
}
