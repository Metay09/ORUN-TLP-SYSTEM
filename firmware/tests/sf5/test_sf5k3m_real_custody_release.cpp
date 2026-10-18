// SF5K3M: real authenticated ACK -> ProductRecordStore -> ObservationStore
// flash release marker, using the previous J actual SecurityStore fixture.
// NO physical radio and NO Gateway ACK signer in this host test.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <vector>

#include "custody_ack_release_owner.h"
#include "product_record_store.h"
#include "tlp_v2_product_secure.h"

#define main sf5k3j_policy_previous_main
#include "test_sf5k3j_real_security_v3_policy.cpp"
#undef main

namespace {
namespace osc=orun_tlp::observation_store_control;
namespace tlp=orun_tlp::tlp;
using Owner=orun_tlp::CustodyAckReleaseOwner;

constexpr uint16_t kProductPages=32U;
constexpr uint32_t kPage=4096U;
const char kValidAck[] =
  "02050164010203040506070823002300230023010000000700000000"
  "0000000192453945bd2224e144572044decab3402105011f5844a453";

uint8_t nibble(char c) {
  if(c>='0'&&c<='9')return static_cast<uint8_t>(c-'0');
  if(c>='a'&&c<='f')return static_cast<uint8_t>(c-'a'+10);
  assert(false); return 0U;
}
void makeAck(uint8_t (&ack)[56]) {
  assert(strlen(kValidAck)==112U);
  for(size_t i=0;i<sizeof(ack);++i)
    ack[i]=static_cast<uint8_t>((nibble(kValidAck[2U*i])<<4)|
                                   nibble(kValidAck[2U*i+1U]));
}
struct ProductFlash final : public FlashBackend {
  std::vector<uint8_t> bytes;
  bool async=false;
  bool pending_=false;
  bool fail_next_program=false;
  unsigned pending_polls=0U;
  ProductFlash():bytes(size_t(kProductPages)*kPage,0xFFU){}
  bool begin() override {return true;}
  bool read(uint32_t off,void* out,size_t size) const override {
    if(out==nullptr||off>bytes.size()||size>bytes.size()-off)return false;
    memcpy(out,bytes.data()+off,size);return true;
  }
  FlashOpResult program(uint32_t off,const void* src,size_t size) override {
    if(pending_||src==nullptr||size==0U||(off&3U)||(size&3U)||
       off>bytes.size()||size>bytes.size()-off)
      return FlashOpResult::kFailed;
    const uint8_t* p=static_cast<const uint8_t*>(src);
    for(size_t i=0;i<size;++i)
      if(bytes[off+i]!=0xFFU)return FlashOpResult::kFailed;
    if(fail_next_program) {
      fail_next_program=false;
      return FlashOpResult::kFailed;
    }
    for(size_t i=0;i<size;++i)bytes[off+i]&=p[i];
    return finish();
  }
  FlashOpResult erasePage(uint32_t page) override {
    if(pending_||page>=kProductPages)return FlashOpResult::kFailed;
    memset(bytes.data()+size_t(page)*kPage,0xFFU,kPage);
    return finish();
  }
  FlashOpResult pollPending() override {
    if(!pending_)return FlashOpResult::kFailed;
    if(pending_polls-- > 0U)return FlashOpResult::kPending;
    pending_=false;return FlashOpResult::kDone;
  }
  FlashOpResult finish(){
    if(!async)return FlashOpResult::kDone;
    pending_=true;pending_polls=2U;
    return FlashOpResult::kPending;
  }
};
struct Source final:ObservationIncarnationSource {
  bool generate(uint64_t& out) override {out=100U;return true;}
};
PeriodicRecordInputs period(){
  PeriodicRecordInputs p{};
  p.period_duration_seconds=3600U;
  p.location.valid=true;
  p.location.latitude_e7=376102718;
  p.location.longitude_e7=280545028;
  p.location.hdop_x100=120U;
  p.location.satellites=7U;
  p.location.age_valid=true;
  p.location.age_seconds=3U;
  p.battery_valid=true;
  p.battery_mv=4100U;
  p.battery_state=1U;
  p.health.valid=true;
  return p;
}
osf::RecordIdentity append(ProductRecordStore& store){
  assert(store.submitPeriodic(period())==ProductRecordStore::Submit::kAccepted);
  ProductRecordStore::Outcome outcome{};
  for(unsigned n=0;n<20000U;++n) {
    store.poll();
    if(store.takeOutcome(outcome)) {
      assert(outcome.stored);
      return {outcome.incarnation,outcome.sequence};
    }
  }
  assert(false);return {};
}
osc::ExactObject makeObject(const osf::RecordIdentity& id) {
  tlp::ProductSecureFrame p{};
  p.app_family=tlp::kProductAppFamilyPeriodic;
  p.path_flags=3U;
  p.security_profile=tlp::kProductSecurityProfileRootAeadV1;
  p.ciphertext_len=tlp::kPeriodicPlaintextSize;
  p.device_id=kDevice;
  p.key_epoch=7U;
  p.security_counter=42U;
  for(size_t i=0;i<tlp::kPeriodicPlaintextSize;++i)
    p.ciphertext[i]=static_cast<uint8_t>(i*5U+7U);
  for(size_t i=0;i<tlp::kProductSecureTagSize;++i)
    p.tag[i]=static_cast<uint8_t>(0xA0U+i);
  osc::ExactObject obj{};
  obj.identity=id;
  obj.record_kind=osf::RecordKind::kPeriodic;
  obj.object_size=tlp::kPeriodicFrameSize;
  assert(tlp::encodeProductSecureFrame(p,obj.object,sizeof(obj.object))==
         tlp::ProductSecureCodecResult::kOk);
  return obj;
}
void cache(ProductRecordStore& store,const osc::ExactObject& obj) {
  assert(store.submitExactCache(obj)==ProductRecordStore::CacheSubmit::kStarted);
  ProductRecordStore::CacheOutcome outcome{};
  for(unsigned n=0;n<30000U;++n){
    store.poll();
    if(store.takeCacheOutcome(outcome)){
      assert(outcome.durable);
      assert(outcome.identity.incarnation==obj.identity.incarnation);
      assert(outcome.identity.sequence==obj.identity.sequence);
      return;
    }
  }
  assert(false);
}
void assertRetained(const ProductRecordStore& store,
                    const osf::RecordIdentity& id,bool expected_released){
  ObservationStore::Record found{};
  assert(store.store().lookup(id,found)==ObservationStore::LookupResult::kFound);
  assert(found.released==expected_released);
  assert(!found.release_uncertain);
}
void testRelease(bool async,bool fail_marker) {
  Flash security_flash;
  SecurityStore security(security_flash,security_flash);
  provisionV3(security_flash,security);
  ProductFlash flash;
  flash.async=async;
  Source source;
  ProductRecordStore records(flash,kProductPages,&source);
  const product_region::Jobs tracker{false,true};
  records.begin(kDevice,tracker);
  assert(records.mounted());
  const osf::RecordIdentity id=append(records);
  cache(records,makeObject(id));
  assertRetained(records,id,false);

  uint8_t ack[56]{};
  makeAck(ack);
  Owner owner(security,records);
  assert(owner.submit(ack,sizeof(ack))==Owner::Submit::kAuthenticationRejected);
  assertRetained(records,id,false);

  assert(Seam::grant(security,0U,kGateA));
  assert(owner.submit(ack,sizeof(ack))==Owner::Submit::kAuthenticationRejected);
  settle(security);
  bool committed=false;
  assert(Seam::take(security,committed)&&committed);
  assert(owner.submit(ack,sizeof(ack))==Owner::Submit::kStarted);
  assert(owner.pending());
  assert(owner.service()==Owner::Service::kWaitingFlash);
  assert(owner.submit(ack,sizeof(ack))==Owner::Submit::kBusy);
  if(fail_marker) flash.fail_next_program=true;

  Owner::Service result=Owner::Service::kWaitingFlash;
  for(unsigned n=0;n<20000U;++n){
    records.poll();
    result=owner.service();
    if(result!=Owner::Service::kWaitingFlash)break;
  }
  assert(!owner.pending());
  assert(owner.service()==Owner::Service::kIdle);
  assert(result==(fail_marker?Owner::Service::kFailed:
                             Owner::Service::kDurablyReleased));
  assertRetained(records,id,!fail_marker);

  Source reboot_source;
  ProductRecordStore reboot(flash,kProductPages,&reboot_source);
  reboot.begin(kDevice,tracker);
  assert(reboot.mounted());
  assertRetained(reboot,id,!fail_marker);
  osc::ExactObject live[osf::kMaxExactCustodyObjects]{};
  uint16_t count=99U;
  assert(reboot.snapshotCustodyCandidates(live,count));
  assert(count==(fail_marker?1U:0U));
  if(!fail_marker){
    // Durable marker, not in-place flash erasure: original object is no
    // longer eligible for custody retry after reboot, even if control
    // bytes physically remain until later safe control compaction.
    Owner repeated(security,reboot);
    assert(repeated.submit(ack,sizeof(ack))!=Owner::Submit::kStarted);
    assert(reboot.diagnostics().custody_release_committed==0U);
  }
}

} // namespace

int main(){
  testRelease(false,false);
  testRelease(true,false);
  testRelease(false,true);
  testRelease(true,true);
  puts("SF5K3M real HMAC ACK -> ProductRecordStore durable release/reset/cuts: PASS");
  return 0;
}
