// KQ05 PC ONLY: actual SecurityStore V3 authenticated 0x0A ACK,
// ProductRecordStore/ObservationStore committed selective release markers.
// Simulated NOR only: no device uploads, live RF, erases or History migration.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "gateway_product_custody_runtime.h"
#include "tracker_ackless_live_tx.h"
#include "gateway_local_custody_owner.h"
#include "gateway_custody_authorized_ingress.h"
#include "kq05_tracker_batch_release_owner.h"
#define ORUN_KQ05_TRACKER_BATCH_RELEASE_HOST_TEST 1
#include "test_kq05_tracker_batch_security_verify_host.cpp"

#define main relay_regression_main
#include "../m7/test_lora_command_relay.cpp"
#undef main

// Only radio delivery, the LIVE sealing chip and the clock are simulated.
// The Gateway signer and Tracker receipt verifier use the real custody HMAC.
namespace {
uint32_t simulation_ms;
orun_tlp::ProductSecureRxFrame uplink;
uint8_t receipt[ack::kMaxSize];
size_t receipt_size;
unsigned downlinks;
uint32_t live_sequence;
}
namespace orun_tlp {
namespace monotonic { uint32_t nowMs() { return simulation_ms; } }
bool RadioManager::canSend() const { return true; }
uint64_t RadioManager::deviceId() const { return kGateA; }
bool RadioManager::takeProductSecureFrame(ProductSecureRxFrame* out) {
  if (!uplink.size) return false;
  *out = uplink;
  uplink = {};
  return true;
}
bool RadioManager::sendAuthorizedCustodyAck(GatewayCustodyAuthorizedIngress&) { return false; }
bool RadioManager::sendAuthorizedBatchAck(GatewayCustodyAuthorizedIngress& in,
    const ProductCustodyStore::Handle* handles, uint8_t n, const GatewayCustodyAckScope& scope) {
  if (!in.prepareAuthorizedBatchAck(handles, n, scope, receipt, receipt_size)) return false;
  ++downlinks;
  return true;
}
bool RadioManager::sendProductSecurePacket(const uint8_t* bytes, size_t n) {
  assert(!uplink.size && n <= sizeof(uplink.payload));
  memcpy(uplink.payload, bytes, n);
  uplink.size = n;
  return true;
}
ProductSecureCryptoResult ProductSecureCrypto::protectNextPeriodic(uint8_t flags,
    const tlp::PeriodicObservationPlaintext& value, uint8_t (&out)[tlp::kPeriodicFrameSize]) {
  assert(flags == 0U);
  osf::RecordIdentity id{};
  id.incarnation = value.record_incarnation;
  id.sequence = value.record_sequence;
  auto object = makeObject(id);
  memcpy(out, object.object, sizeof(out));
  out[4] = flags;
  out[27] = 99U;
  live_sequence = id.sequence;
  return ProductSecureCryptoResult::kOk;
}
}
namespace {
using ReleaseOwner=orun_tlp::Kq05TrackerBatchReleaseOwner;

void assertFiveLogicalFlags(const Fixture& f,
                            const bool (&released)[5]) {
  for (uint8_t i=0U;i<5U;++i) {
    ObservationStore::Record record{};
    assert(f.records.store().lookup(f.objects[i].identity,record)==
           ObservationStore::LookupResult::kFound);
    assert(record.released==released[i]);
    assert(!record.release_uncertain);
  }
}

ReleaseOwner::Service nextDurableOutcome(ReleaseOwner& owner,
                                        ProductRecordStore& records) {
  for (unsigned iteration=0U;iteration<30000U;++iteration) {
    records.poll();
    const auto result=owner.service();
    if (result != ReleaseOwner::Service::kWaitingFlash &&
        result != ReleaseOwner::Service::kWaitingStorage)
      return result;
  }
  assert(false && "Flash release marker did not reach durable outcome");
  return ReleaseOwner::Service::kFailed;
}

// Pass bytes signed by the production gateway owner to the production tracker
// verifier. A hole in the received sequence must survive both delivery and reboot.
void testGatewayToTrackerWithMissingRecord() {
  using namespace orun_tlp;
  struct Grant final : GatewayCustodyGrantProvider {
    bool activeGrant(uint64_t tracker, uint32_t epoch, GatewayCustodyAckScope& scope,
                     uint8_t (&key)[32]) override {
      scope = allowedScope();
      if (scope.tracker_device_id != tracker || scope.key_epoch != epoch) return false;
      fromHex32(key);
      return true;
    }
  } grant;

  Fixture tracker;
  tracker.admitGrant();
  tracker.cacheUpTo(5);
  ProductFlash flash;
  flash.async = true;
  ProductCustodyStore store(flash, kProductPages);
  assert(store.begin());
  GatewayLocalCustodyOwner gateway(store);
  GatewayCustodyAuthorizedIngress ingress(gateway, grant, kGateA);
  const unsigned delivered[] = {0, 1, 3};
  RadioManager radio;
  GatewayProductCustodyRuntime runtime(radio, ingress);
  GatewayProductLiveIngress live_ingress(store, grant, kGateA);
  runtime.attachLiveIngress(live_ingress);
  LoraCommandRelay relay;
  auto step = [&]() {
    runtime.reserveCommandWindow(relay.windowReserved(simulation_ms));
    return runtime.update(true);
  };
  auto offer = [&](unsigned index) {
    const auto& exact = tracker.objects[index];
    assert(radio.sendProductSecurePacket(exact.object, exact.object_size));
    step();
    for (unsigned n = 0; n < 150; ++n) step();
  };
  for (unsigned index : delivered) {
    offer(index);
    simulation_ms += 60000U;
  }
  assert(downlinks == 0U && runtime.reservedPages() != 0U);
  simulation_ms = 301000U;
  step();  // No TX while the collar sleeps.
  assert(downlinks == 0U);
  offer(0);  // Another RX opportunity.
  assert(downlinks == 1U && runtime.reservedPages() == 0U);
  // Lose this downlink. The next fresh position must still take the radio.
  for (const auto& object : tracker.objects) assertRetained(tracker.records, object.identity, false);
  ProductSecureCrypto sealer(tracker.security);
  TrackerAcklessLiveTx live_sender(tracker.records, sealer, radio);
  assert(live_sender.update(simulation_ms, true) == TrackerAcklessLiveTx::Event::kSent);
  assert(live_sequence == tracker.objects[4].identity.sequence);
  assert(step() == GatewayProductCustodyRuntime::Event::kCustodyStarted);
  for (unsigned n = 0; n < 150 && live_ingress.pending(); ++n) step();
  assert(!live_ingress.pending() && downlinks == 1U);
  for (unsigned index : delivered) offer(index);
  simulation_ms += 301000U;
  const auto setting = command(kDevice, 17);
  assert(relay.queue(setting.data(), setting.size()) == LoraCommandRelay::Queued::kAdded);
  relay.heard(kDevice, simulation_ms);
  offer(0);
  simulation_ms += LoraCommandRelay::kAckGraceMs;
  LoraCommandRelay::Frame packet{};
  assert(relay.due(simulation_ms, packet) && packet.command_id == 17);
  step();
  assert(downlinks == 1U);
  const auto response = result(kDevice, 17);
  assert(relay.answered(response.data(), response.size()));
  step();
  assert(downlinks == 2U);
  ack::Header header{};
  assert(ack::decode(receipt, receipt_size, header) && header.count == 3U);
  ReleaseOwner release(tracker.security, tracker.records);
  assert(release.submit(receipt, receipt_size) == ReleaseOwner::Submit::kStarted);
  for (unsigned i = 0; i < 3; ++i) {
    assert(release.service() == ReleaseOwner::Service::kWaitingFlash);
    assert(nextDurableOutcome(release, tracker.records) == ReleaseOwner::Service::kReleasedOne);
  }
  assert(release.service() == ReleaseOwner::Service::kApplied);
  const bool released[] = {true, true, false, true, false};
  ProductRecordStore reboot(tracker.product_flash, kProductPages, &tracker.source);
  reboot.begin(kDevice, product_region::Jobs{false, true});
  for (unsigned i = 0; i < 5; ++i) assertRetained(reboot, tracker.objects[i].identity, released[i]);
  puts("Gateway durable signed receipt -> Tracker selective release with gap and reboot: PASS");
}

void testFiveExactIndependentDurableMarkers() {
  for (uint8_t count=1U;count<=5U;++count) {
    Fixture f;
    f.admitGrant();
    f.cacheUpTo(5U);

    uint8_t wire[ack::kMaxSize]{};
    size_t length=0U;
    buildAck(f.objects,count,wire,length);
    ReleaseOwner owner(f.security,f.records);
    assert(owner.submit(wire,length)==ReleaseOwner::Submit::kStarted);
    assert(owner.pending());
    assert(owner.releasedCount()==0U);

    bool expected[5]{};
    assertFiveLogicalFlags(f,expected);
    assert(owner.submit(wire,length)==ReleaseOwner::Submit::kBusy);

    uint32_t sequences[5]{};
    AuthenticatedKq05TrackerBatchAck proof{};
    assert(f.verifier.verifyFromStore(wire,length,f.records,proof)==
           Kq05TrackerBatchVerifyResult::kAuthenticated);
    for (uint8_t j=0U;j<count;++j) sequences[j]=ProofPeer::sequence(proof,j);

    for (uint8_t i=0U;i<count;++i) {
      assert(owner.service()==ReleaseOwner::Service::kWaitingFlash);
      assert(owner.releasedCount()==i);
      // Release request is NOT a durable result until ProductRecordStore
      // independently completes the physical simulated NOR marker.
      const auto result=nextDurableOutcome(owner,f.records);
      assert(result==ReleaseOwner::Service::kReleasedOne);
      assert(owner.releasedCount()==i+1U);
      expected[sequences[i]-1U]=true;
      assertFiveLogicalFlags(f,expected);
    }
    assert(owner.service()==ReleaseOwner::Service::kApplied);
    assert(!owner.pending());
    assert(owner.releasedCount()==count);
    assert(owner.service()==ReleaseOwner::Service::kIdle);
    // Replaying the same complete ACK cannot release anything else.
    const auto replay=owner.submit(wire,length);
    assert(replay!=ReleaseOwner::Submit::kStarted);
    assertFiveLogicalFlags(f,expected);

    ProductRecordStore reboot(f.product_flash,kProductPages,&f.source);
    reboot.begin(kDevice,product_region::Jobs{false,true});
    assert(reboot.mounted());
    for (uint8_t j=0U;j<5U;++j) {
      ObservationStore::Record after{};
      assert(reboot.store().lookup(f.objects[j].identity,after)==
             ObservationStore::LookupResult::kFound);
      assert(after.released==expected[j] && !after.release_uncertain);
    }
  }
}

void testNoGrantTamperedOrIncompleteGroupNeverStartsFlash() {
  Fixture f;
  f.cacheUpTo(5U);
  uint8_t wire[ack::kMaxSize]{};
  size_t length=0U;
  buildAck(f.objects,5U,wire,length);
  ReleaseOwner owner(f.security,f.records);
  assert(owner.submit(wire,length)==
         ReleaseOwner::Submit::kAuthenticationRejected);
  assert(!owner.pending());
  bool untouched[5]{};
  assertFiveLogicalFlags(f,untouched);

  f.admitGrant();
  uint8_t bad[ack::kMaxSize]{};
  memcpy(bad,wire,length);
  bad[length-1U]^=0x01U;
  assert(owner.submit(bad,length)==
         ReleaseOwner::Submit::kAuthenticationRejected);
  assert(!owner.pending());
  assertFiveLogicalFlags(f,untouched);
  assert(owner.submit(wire,length-1U)==
         ReleaseOwner::Submit::kAuthenticationRejected);
  assertFiveLogicalFlags(f,untouched);
}

void testFailureOnSecondMarkerNeverReleasesLaterRecords() {
  Fixture f;
  f.admitGrant();
  f.cacheUpTo(5U);
  uint8_t wire[ack::kMaxSize]{};
  size_t length=0U;
  buildAck(f.objects,5U,wire,length);

  AuthenticatedKq05TrackerBatchAck proof{};
  assert(f.verifier.verifyFromStore(wire,length,f.records,proof)==
         Kq05TrackerBatchVerifyResult::kAuthenticated);
  const uint32_t first=ProofPeer::sequence(proof,0U);
  const uint32_t second=ProofPeer::sequence(proof,1U);

  ReleaseOwner owner(f.security,f.records);
  assert(owner.submit(wire,length)==ReleaseOwner::Submit::kStarted);
  assert(owner.service()==ReleaseOwner::Service::kWaitingFlash);
  assert(nextDurableOutcome(owner,f.records)==
         ReleaseOwner::Service::kReleasedOne);
  assert(owner.releasedCount()==1U);

  // Inject the failure only when the SECOND separate marker starts.
  f.product_flash.fail_next_program=true;
  assert(owner.service()==ReleaseOwner::Service::kWaitingFlash);
  assert(nextDurableOutcome(owner,f.records)==
         ReleaseOwner::Service::kFailed);
  assert(!owner.pending() && owner.releasedCount()==1U);
  bool expected[5]{};
  expected[first-1U]=true;
  assertFiveLogicalFlags(f,expected);
  assert(second!=first);
  assert(f.records.diagnostics().custody_release_committed==1U);
  assert(f.records.diagnostics().custody_release_failed==1U);

  ProductRecordStore reboot(f.product_flash,kProductPages,&f.source);
  reboot.begin(kDevice,product_region::Jobs{false,true});
  assert(reboot.mounted());
  for (uint8_t i=0U;i<5U;++i) {
    ObservationStore::Record rec{};
    assert(reboot.store().lookup(f.objects[i].identity,rec)==
           ObservationStore::LookupResult::kFound);
    assert(rec.released==expected[i]);
  }
}

void testRevocationBetweenMarkersStopsRemaining() {
  Fixture f;
  f.admitGrant();
  f.cacheUpTo(5U);
  uint8_t wire[ack::kMaxSize]{};
  size_t length=0U;
  buildAck(f.objects,5U,wire,length);
  AuthenticatedKq05TrackerBatchAck proof{};
  assert(f.verifier.verifyFromStore(wire,length,f.records,proof)==
         Kq05TrackerBatchVerifyResult::kAuthenticated);
  const uint32_t first=ProofPeer::sequence(proof,0U);

  ReleaseOwner owner(f.security,f.records);
  assert(owner.submit(wire,length)==ReleaseOwner::Submit::kStarted);
  assert(owner.service()==ReleaseOwner::Service::kWaitingFlash);
  assert(nextDurableOutcome(owner,f.records)==
         ReleaseOwner::Service::kReleasedOne);

  assert(Seam::revoke(f.security,0U,kGateA));
  settle(f.security);
  bool revoked=false;
  assert(Seam::take(f.security,revoked) && revoked);
  assert(owner.service()==ReleaseOwner::Service::kFailed);
  assert(!owner.pending());
  bool expected[5]{};
  expected[first-1U]=true;
  assertFiveLogicalFlags(f,expected);
}

void testCrashAfterOneMarkerRequiresNewRemainingSubsetAck() {
  Fixture f;
  f.admitGrant();
  f.cacheUpTo(5U);
  uint8_t full[ack::kMaxSize]{};
  size_t full_length=0U;
  buildAck(f.objects,5U,full,full_length);
  AuthenticatedKq05TrackerBatchAck proof{};
  assert(f.verifier.verifyFromStore(full,full_length,f.records,proof)==
         Kq05TrackerBatchVerifyResult::kAuthenticated);
  const uint32_t first=ProofPeer::sequence(proof,0U);

  {
    ReleaseOwner interrupted(f.security,f.records);
    assert(interrupted.submit(full,full_length)==
           ReleaseOwner::Submit::kStarted);
    assert(interrupted.service()==ReleaseOwner::Service::kWaitingFlash);
    assert(nextDurableOutcome(interrupted,f.records)==
           ReleaseOwner::Service::kReleasedOne);
    // Simulated power cut: only owner RAM disappears, Flash survives.
  }

  ProductRecordStore reboot(f.product_flash,kProductPages,&f.source);
  reboot.begin(kDevice,product_region::Jobs{false,true});
  assert(reboot.mounted());
  SecurityStore security_reboot(f.security_flash,f.security_flash);
  assert(security_reboot.begin(DeviceIdentity::fromLegacyUint64(kDevice)));
  settle(security_reboot);
  ReleaseOwner recovered(security_reboot,reboot);
  // Full original batch is no longer re-authenticable from five retained
  // ciphertexts, so NEVER guess or skip a released identity.
  assert(recovered.submit(full,full_length)!=ReleaseOwner::Submit::kStarted);
  ObservationStore::Record previously{};
  assert(reboot.store().lookup(f.objects[first-1U].identity,previously)==
         ObservationStore::LookupResult::kFound);
  assert(previously.released);

  // Gateway may issue a *new MAC* for the remaining four exact objects
  // after its own custody readback (simulated here by the trusted fixture).
  osc::ExactObject remaining[4]{};
  uint8_t count=0U;
  for (uint8_t i=0U;i<5U;++i) {
    if (f.objects[i].identity.sequence==first) continue;
    remaining[count++]=f.objects[i];
  }
  assert(count==4U);
  uint8_t subset[ack::kMaxSize]{};
  size_t subset_length=0U;
  buildAck(remaining,count,subset,subset_length);
  assert(recovered.submit(subset,subset_length)==
         ReleaseOwner::Submit::kStarted);
  for (uint8_t i=0U;i<count;++i) {
    assert(recovered.service()==ReleaseOwner::Service::kWaitingFlash);
    assert(nextDurableOutcome(recovered,reboot)==
           ReleaseOwner::Service::kReleasedOne);
  }
  assert(recovered.service()==ReleaseOwner::Service::kApplied);
  for (uint8_t i=0U;i<5U;++i) {
    ObservationStore::Record rec{};
    assert(reboot.store().lookup(f.objects[i].identity,rec)==
           ObservationStore::LookupResult::kFound);
    assert(rec.released);
  }
}

} // namespace

#ifndef ORUN_KQ05_TRACKER_BATCH_RADIO_RUNTIME_HOST_TEST
int main() {
  // Run the inherited real SecurityStore full-digest/HMAC regressions too.
  // They are intentionally included in this translation unit; do not
  // suppress -Werror or leave security tests compiled but unused.
  testGatewayToTrackerWithMissingRecord();
  testActualRootDerivedFiveAndSubsets();
  testTamperAndRevocationDoNotPublishCapability();
  testFiveExactIndependentDurableMarkers();
  testNoGrantTamperedOrIncompleteGroupNeverStartsFlash();
  testFailureOnSecondMarkerNeverReleasesLaterRecords();
  testRevocationBetweenMarkersStopsRemaining();
  testCrashAfterOneMarkerRequiresNewRemainingSubsetAck();
  puts("KQ05 Tracker 1..5 REAL V3 HMAC -> selective durable OBS1 marker, failure/reboot: PASS (HOST ONLY)");
  puts("NO LORA TX/RX, NO REAL FLASH, NO WHOLE REGION ERASE, NO DEVICE UPLOAD");
  return 0;
}
#endif  // ORUN_KQ05_TRACKER_BATCH_RADIO_RUNTIME_HOST_TEST
