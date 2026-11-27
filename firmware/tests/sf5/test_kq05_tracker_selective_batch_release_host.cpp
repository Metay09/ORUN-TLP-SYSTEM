// KQ05 PC ONLY: actual SecurityStore V3 authenticated 0x0A ACK,
// ProductRecordStore/ObservationStore committed selective release markers.
// Simulated NOR only: no device uploads, live RF, erases or History migration.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "gateway_product_custody_runtime.h"
#include "tracker_critical_event_tx.h"
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
bool radio_busy;
unsigned prepare_attempts;
uint32_t prepare_delay_ms;
unsigned defer_starts;
int stale_slot = -1;
uint32_t live_sequence;
unsigned seals;
bool sealed_with_fix;
const orun_tlp::RadioManager* collar_radio;
}
namespace orun_tlp {

struct RadioBudgetHostProbe {
  static void consume(
      RadioManager& radio,
      uint32_t now, uint8_t size) {
    assert(radio.rf_budget_.reserve(now, size));
    radio.rf_budget_.finish(
        now + RfDutyBudget::costMs(size));
  }
};

namespace monotonic { uint32_t nowMs() { return simulation_ms; } }
bool RadioManager::canSend() const { return !radio_busy; }
uint64_t RadioManager::deviceId() const { return this == collar_radio ? kDevice : kGateA; }
bool RadioManager::takeProductSecureFrame(ProductSecureRxFrame* out) {
  if (!uplink.size) return false;
  *out = uplink;
  out->rx_floor_ms = simulation_ms;
  uplink = {};
  return true;
}
bool RadioManager::sendAuthorizedCustodyAck(GatewayCustodyAuthorizedIngress&) { return false; }
RadioManager::BatchTx RadioManager::sendAuthorizedBatchAck(GatewayCustodyAuthorizedIngress& in,
    const ProductCustodyStore::Handle* handles, uint8_t n, const GatewayCustodyAckScope& scope,
    uint32_t heard) {
  ++prepare_attempts;
  if (defer_starts) {
    --defer_starts;
    return BatchTx::kDeferred;
  }
  simulation_ms += prepare_delay_ms;
  ProductCustodyStore::Handle hints[ack::kMaxObjects]{};
  for (unsigned i = 0; i < n; ++i) {
    hints[i] = handles[i];
    if (hints[i].slot == stale_slot) ++hints[i].generation;
  }
  bool stale = false;
  if (!in.prepareAuthorizedBatchAck(hints, n, scope, receipt, receipt_size, &stale))
    return stale ? BatchTx::kStaleHandle : BatchTx::kUnverifiable;
  if (!ackFitsWindow(simulation_ms, heard, receipt_size) ||
      !gatewayAirtimeAvailable(receipt_size)) return BatchTx::kDeferred;
  if (!rf_budget_.reserve(simulation_ms, receipt_size))
    return BatchTx::kDeferred;
  rf_budget_.finish(simulation_ms);
  ++downlinks;
  return BatchTx::kStarted;
}
bool RadioManager::sendProductSecurePacket(const uint8_t* bytes, size_t n) {
  assert(!uplink.size && n <= sizeof(uplink.payload));
  memcpy(uplink.payload, bytes, n);
  uplink.size = n;
  return true;
}
ProductSecureCryptoResult ProductSecureCrypto::protectNextPeriodic(uint8_t flags,
    const tlp::PeriodicObservationPlaintext& value, uint8_t (&out)[tlp::kPeriodicFrameSize]) {
  assert(flags == 0 || flags == 3);
  ++seals;
  sealed_with_fix = (value.record_flags & tlp::kPeriodicLocationValid) != 0;
  osf::RecordIdentity id{};
  id.incarnation = value.record_incarnation;
  id.sequence = value.record_sequence;
  auto object = makeObject(id);
  memcpy(out, object.object, sizeof(out));
  out[4] = flags;
  out[27] = static_cast<uint8_t>(99U + seals);
  live_sequence = id.sequence;
  return ProductSecureCryptoResult::kOk;
}
ProductSecureCryptoResult ProductSecureCrypto::protectNextEvent(uint8_t flags,
    const tlp::EventPlaintext& event, uint8_t (&out)[tlp::kEventFrameSize]) {
  tlp::ProductSecureFrame frame{};
  frame.app_family = tlp::kProductAppFamilyEvent;
  frame.ciphertext_len = tlp::kEventPlaintextSize;
  frame.path_flags = flags;
  frame.device_id = kDevice;
  frame.key_epoch = 7;
  frame.security_counter = event.record_sequence + 200;
  assert(tlp::encodeProductSecureFrame(frame, out, sizeof(out)) == tlp::ProductSecureCodecResult::kOk);
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

struct Grant final : GatewayCustodyGrantProvider {
  bool enabled = true;
  bool activeGrant(uint64_t tracker, uint32_t epoch, GatewayCustodyAckScope& scope,
                   uint8_t (&key)[32]) override {
    scope = allowedScope();
    if (!enabled || scope.tracker_device_id != tracker || scope.key_epoch != epoch) return false;
    fromHex32(key);
    return true;
  }
};

// Pass bytes signed by the production gateway owner to the production tracker
// verifier. A hole in the received sequence must survive both delivery and reboot.
void testGatewayToTrackerWithMissingRecord() {
  using namespace orun_tlp;
  Grant grant;

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
  radio_busy = true;  // unavailable during the first three listen opportunities
  for (unsigned index : delivered) {
    offer(index);
    simulation_ms += 60000U;
  }
  assert(downlinks == 0U && runtime.reservedPages() != 0U);
  radio_busy = false;
  simulation_ms = 301000U;
  step();  // No TX while the collar sleeps.
  assert(downlinks == 0U);
  offer(0);  // Another RX opportunity.
  assert(downlinks == 1U && runtime.reservedPages() == 0U);
  // Lose this downlink. The next fresh position must still take the radio.
  for (const auto& object : tracker.objects) assertRetained(tracker.records, object.identity, false);
  ProductSecureCrypto sealer(tracker.security);
  const auto overflow = append(tracker.records);
  RadioManager collar;
  collar_radio = &collar;
  TrackerCriticalEventTx periodic(tracker.records, sealer, collar, TrackerCriticalEventTx::Mode::kPeriodicHistory);
  assert(periodic.update(simulation_ms, true) == TrackerCriticalEventTx::Result::kSent);
  assert(live_sequence == overflow.sequence);
  assert(step() == GatewayProductCustodyRuntime::Event::kCustodyStarted);
  for (unsigned n = 0; n < 150 && live_ingress.pending(); ++n) step();
  assert(!live_ingress.pending() && downlinks == 1U);
  radio_busy = true;
  for (unsigned index : delivered) offer(index);
  radio_busy = false;
  simulation_ms += 301000U;
  const auto setting = command(kDevice, 17);
  assert(relay.queue(setting.data(), setting.size()) == LoraCommandRelay::Queued::kAdded);
  relay.heard(kDevice, simulation_ms);
  offer(0);
  simulation_ms += LoraCommandRelay::kAckGraceMs;
  LoraCommandRelay::Frame packet{};
  assert(!relay.due(simulation_ms, packet, [](uint8_t) { return false; }));
  assert(relay.due(simulation_ms, packet) && packet.command_id == 17 && packet.attempt == 1);
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

void testOneStaleHandleDoesNotPoisonItsPeer() {
  using namespace orun_tlp;
  Fixture tracker;
  Grant grant;
  ProductFlash flash;
  ProductCustodyStore store(flash, kProductPages);
  assert(store.begin());
  GatewayLocalCustodyOwner owner(store);
  GatewayCustodyAuthorizedIngress ingress(owner, grant, kGateA);
  RadioManager radio;
  collar_radio = nullptr;
  GatewayProductCustodyRuntime runtime(radio, ingress);
  simulation_ms = 40000;
  downlinks = 0;
  for (unsigned i = 0; i < 2; ++i) {
    const auto& obj = tracker.objects[i];
    assert(radio.sendProductSecurePacket(obj.object, obj.object_size));
    for (unsigned n = 0; n < 150; ++n) runtime.update(true);
  }
  stale_slot = 0;
  simulation_ms += 1000;
  for (unsigned i = 0; i < 3; ++i) {
    runtime.update(true);
    simulation_ms += 500;
  }
  ack::Header header{};
  assert(downlinks == 1 && ack::decode(receipt, receipt_size, header) && header.count == 1);
  assert(runtime.reservedPages() == 0);
  tracker.admitGrant();
  tracker.cacheUpTo(2);
  ReleaseOwner release(tracker.security, tracker.records);
  assert(release.submit(receipt, receipt_size) == ReleaseOwner::Submit::kStarted);
  while (release.pending()) {
    tracker.records.poll();
    assert(release.service() != ReleaseOwner::Service::kFailed);
  }
  assertRetained(tracker.records, tracker.objects[0].identity, false);
  assertRetained(tracker.records, tracker.objects[1].identity, true);
  stale_slot = -1;
}

void testLongOutageUsesEveryExactSlot() {
  using namespace orun_tlp;
  Fixture tracker;
  RadioManager radio;
  collar_radio = &radio;
  ProductSecureCrypto crypto(tracker.security);
  using Sender = TrackerCriticalEventTx;
  Sender periodic(tracker.records, crypto, radio, Sender::Mode::kPeriodicHistory);
  ProductSecureRxFrame packets[4]{};
  unsigned attempts[4]{};
  uint32_t last[4]{};
  unsigned unique = 0;
  for (uint32_t now = 1000; now <= 1201000; now += 5000) {
    for (unsigned poll = 0; poll < 120; ++poll) {
      tracker.records.poll();
      if (periodic.update(now, true) != Sender::Result::kSent) continue;
      assert(uplink.size == 100 && uplink.payload[4] == 3);
      unsigned slot = 0;
      while (slot < unique && memcmp(packets[slot].payload, uplink.payload, uplink.size)) ++slot;
      if (slot == unique) {
        assert(unique < 4);
        packets[unique++] = uplink;
      } else {
        assert(now - last[slot] >= Sender::kReceiptWaitMs);
      }
      last[slot] = now;
      ++attempts[slot];
      uplink = {};
      break;
    }
  }
  assert(unique == 4);
  for (auto n : attempts) assert(n >= 3 && n <= 4);
  collar_radio = nullptr;
}

void testReservedEventSlotAndNoFix() {
  using namespace orun_tlp;
  Fixture f;
  f.cacheUpTo(4);
  // Seed a committed EVENT with the same OBS1 format used by the producer,
  // then reopen the product store as on a power cycle.
  ObservationStore writer(f.product_flash, kProductPages, &f.source);
  assert(writer.begin(kDevice));
  osf::RecordIdentity id{};
  assert(writer.peekNextIdentity(id));
  tlp::EventPlaintext event{};
  event.record_incarnation = id.incarnation;
  event.record_sequence = id.sequence;
  event.event_type = tlp::kEventTypeGeofenceOutside;
  event.lifecycle_state = tlp::kEventLifecycleActive;
  event.severity = tlp::kEventSeverityCritical;
  event.occurrence_id = 1;
  uint8_t payload[tlp::kEventPlaintextSize]{};
  assert(tlp::encodeEventPlaintext(event, payload, sizeof(payload)) == tlp::ProductSecureCodecResult::kOk);
  assert(writer.requestAppend(osf::RecordKind::kEvent, 1, payload, sizeof(payload)) ==
         ObservationStore::AppendResult::kStarted);
  while (writer.busy()) writer.poll();
  ObservationStore::Handle handle{};
  bool stored = false;
  assert(writer.takeAppendResult(stored, handle) && stored);

  ProductRecordStore records(f.product_flash, kProductPages, &f.source);
  records.begin(kDevice, product_region::Jobs{false, true});
  auto no_fix = period();
  no_fix.location = {};
  assert(records.submitPeriodic(no_fix) == ProductRecordStore::Submit::kAccepted);
  ProductRecordStore::Outcome outcome{};
  for (unsigned n = 0; n < 200 && !records.takeOutcome(outcome); ++n) records.poll();
  assert(outcome.stored);
  RadioManager radio;
  collar_radio = &radio;
  ProductSecureCrypto crypto(f.security);
  using Sender = TrackerCriticalEventTx;
  Sender critical(records, crypto, radio), history(records, crypto, radio, Sender::Mode::kPeriodicHistory);
  for (unsigned n = 0; n < 200 && !uplink.size; ++n) {
    records.poll();
    const auto sent = critical.update(1000, true);
    history.update(1000, sent != Sender::Result::kSent);
  }
  assert(uplink.size == 84 && uplink.payload[4] == 3);
  const auto first = uplink;
  uplink = {};
  assert(history.update(15500, true) == Sender::Result::kSent);
  assert(uplink.size == 100 && uplink.payload[4] == 0 && !sealed_with_fix &&
         live_sequence == outcome.sequence);
  uplink = {};
  assert(critical.update(60999, true) != Sender::Result::kSent);
  assert(critical.update(61000, true) == Sender::Result::kSent);
  assert(uplink.size == first.size && !memcmp(uplink.payload, first.payload, first.size));
  uplink = {};
  collar_radio = nullptr;
}

void testFailedWindowsRecover(unsigned count) {
  using namespace orun_tlp;
  Fixture tracker;
  tracker.admitGrant();
  tracker.cacheUpTo(count);
  Grant grant;
  ProductFlash flash;
  ProductCustodyStore store(flash, kProductPages);
  assert(store.begin());
  GatewayLocalCustodyOwner owner(store);
  GatewayCustodyAuthorizedIngress ingress(owner, grant, kGateA);
  RadioManager gw;
  collar_radio = nullptr;
  GatewayProductCustodyRuntime runtime(gw, ingress);
  downlinks = prepare_attempts = 0;
  simulation_ms = 50000;
  // Four/five consecutive durable admissions while the downlink is busy.
  radio_busy = true;
  auto offer = [&](GatewayProductCustodyRuntime& loop, unsigned i) {
    const auto& object = tracker.objects[i];
    assert(gw.sendProductSecurePacket(object.object, object.object_size));
    for (unsigned n = 0; n < 150; ++n) loop.update(true);
    assert(!uplink.size);
  };
  for (unsigned i = 0; i < count; ++i) offer(runtime, i);
  assert(runtime.reservedPages() && !downlinks && !prepare_attempts);
  radio_busy = false;
  // Delay between successful signer admission and start; no loop-rate retries.
  prepare_delay_ms = 9000;
  runtime.update(true);
  assert(prepare_attempts == 1 && !downlinks && runtime.reservedPages());
  prepare_delay_ms = 0;
  for (unsigned n = 0; n < 500; ++n) runtime.update(true);
  assert(prepare_attempts == 1);
  simulation_ms += 60000;
  defer_starts = 1;
  offer(runtime, 0); // duplicate reopens the window without adding a reference
  assert(!downlinks && prepare_attempts == 2 && runtime.reservedPages());
  simulation_ms += 499;
  runtime.update(true);
  assert(!downlinks);
  ++simulation_ms;
  runtime.update(true);
  ack::Header header{};
  assert(downlinks == 1 && !runtime.reservedPages());
  assert(ack::decode(receipt, receipt_size, header) && header.count == count);
  // Lose this entire ACK and reboot Gateway. Rebuild from the durable object.
  for (unsigned i = 0; i < count; ++i)
    assertRetained(tracker.records, tracker.objects[i].identity, false);
  ProductCustodyStore reopened(flash, kProductPages);
  assert(reopened.begin());
  GatewayLocalCustodyOwner next_owner(reopened);
  GatewayCustodyAuthorizedIngress next_ingress(next_owner, grant, kGateA);
  GatewayProductCustodyRuntime next(gw, next_ingress);
  assert(!next.reservedPages());
  simulation_ms += 60000;
  offer(next, 0);
  grant.enabled = false;
  simulation_ms += 1000;
  next.update(true);
  assert(downlinks == 1 && next.reservedPages());
  for (unsigned n = 0; n < 100; ++n) {
    simulation_ms += 100;
    next.update(true);
  }
  assert(prepare_attempts <= 9 && downlinks == 1);
  grant.enabled = true;
  simulation_ms += 60000;
  offer(next, 0);
  assert(downlinks == 2 && ack::decode(receipt, receipt_size, header) && header.count == 1);
  ReleaseOwner release(tracker.security, tracker.records);
  assert(release.submit(receipt, receipt_size) == ReleaseOwner::Submit::kStarted);
  while (release.pending()) {
    tracker.records.poll();
    assert(release.service() != ReleaseOwner::Service::kFailed);
  }
  for (unsigned i = 0; i < count; ++i)
    assertRetained(tracker.records, tracker.objects[i].identity, i == 0);
}

// Clocked two-report trace. Move the RF clock by the actual SF11 airtime;
// the host models zero flash/crypto execution time, and polls every 100 ms.
void reportWindowTrace() {
  using namespace orun_tlp;
  Fixture tracker(1);
  tracker.admitGrant();
  Grant grant;
  ProductFlash flash;
  ProductCustodyStore store(flash, kProductPages);
  assert(store.begin());
  GatewayLocalCustodyOwner owner(store);
  GatewayCustodyAuthorizedIngress ingress(owner, grant, kGateA);
  RadioManager gw, collar;
  collar_radio = &collar;
  GatewayProductCustodyRuntime runtime(gw, ingress);
  ProductSecureCrypto crypto(tracker.security);
  TrackerCriticalEventTx sender(tracker.records, crypto, collar,
                                TrackerCriticalEventTx::Mode::kPeriodicHistory);
  ReleaseOwner release(tracker.security, tracker.records);
  gw.enableGatewayBudget();
  unsigned data = 0, singles = 0, groups = 0, released = 0;
  uint32_t rx_done = 0, ack_done = 0, last_ack_end = 0, newest_first = UINT32_MAX;
  downlinks = 0;
  for (uint32_t tick = 30000; tick <= 520000; tick += 100) {
    simulation_ms = tick;
    if (tick == 90000) append(tracker.records);
    if (ack_done && tick >= ack_done) {
      assert(release.submit(receipt, receipt_size) == ReleaseOwner::Submit::kStarted);
      while (release.pending()) {
        tracker.records.poll();
        if (release.service() == ReleaseOwner::Service::kReleasedOne) ++released;
      }
      last_ack_end = ack_done;
      ack_done = 0;
    }
    if (!rx_done && !ack_done) {
      for (unsigned poll = 0; poll < 128; ++poll) {
        tracker.records.poll();
        if (sender.update(tick, true) != TrackerCriticalEventTx::Result::kSent) continue;
        if (data == 1) newest_first = tick - 90000;
        ++data;
        rx_done = tick + 2216;
        break;
      }
    }
    if (rx_done && tick < rx_done) continue;
    if (rx_done) { simulation_ms = rx_done; rx_done = 0; }
    for (unsigned poll = 0; poll < 128 && !ack_done; ++poll) {
      if (runtime.update(true) != GatewayProductCustodyRuntime::Event::kAckTxStarted) continue;
      ack::Header h{};
      assert(ack::decode(receipt, receipt_size, h) && h.count == 1);
      ++singles;
      ack_done = simulation_ms + 1397;
    }
    if (released == 2) break;
  }
  assert(released == 2 && newest_first == 0 && data == 2);
  printf("TRACE data=%u repeat=%u single=%u group=%u release=%u last_ack_after_second_rx_ms=%lu airtime_us=%lu newest_first_ms=%lu\n",
         data, data - 2, singles, groups, released,
         (unsigned long)(last_ack_end - 92216),
         (unsigned long)(data * 2215936U + singles * 1396736U), (unsigned long)newest_first);
  collar_radio = nullptr;
}

void testOneTransmissionForFreshAndHistory() {
  using namespace orun_tlp;
  Fixture tracker;
  tracker.admitGrant();
  RadioManager collar;
  collar_radio = &collar;
  ProductSecureCrypto crypto(tracker.security);
  using Sender = TrackerCriticalEventTx;
  Sender sender(tracker.records, crypto, collar, Sender::Mode::kPeriodicHistory);
  const auto waitForTx = [&](uint32_t now) {
    for (unsigned poll = 0; poll < 2000; ++poll) {
      tracker.records.poll();
      if (sender.update(now, true) == Sender::Result::kSent) return;
    }
    assert(false && "expected position transmission");
  };

  // Five measurements already in OBS1. Latest goes first, with a receipt
  // request on its very first uplink. The history scan may send older IDs.
  const unsigned before = seals;
  waitForTx(1000U);
  assert(live_sequence == 5 && uplink.payload[4] == 3);
  const auto first = uplink;
  uplink = {};
  waitForTx(61000U);
  assert(live_sequence == 1 && seals == before + 2);
  uplink = {};

  // Gateway signs the exact frame from the first send. Release #5 now,
  // before even one history retransmission of that measurement.
  osc::ExactObject exact{};
  assert(tracker.records.findCachedExact(tracker.objects[4].identity, exact) ==
         ObservationStore::ControlLookupResult::kFound);
  assert(exact.object_size == first.size && !memcmp(exact.object, first.payload, first.size));
  uint8_t wire[ack::kMaxSize]{};
  size_t length = 0;
  buildAck(&exact, 1, wire, length);
  ReleaseOwner owner(tracker.security, tracker.records);
  assert(owner.submit(wire, length) == ReleaseOwner::Submit::kStarted);
  assert(owner.service() == ReleaseOwner::Service::kWaitingFlash);
  assert(nextDurableOutcome(owner, tracker.records) == ReleaseOwner::Service::kReleasedOne);
  assert(owner.service() == ReleaseOwner::Service::kApplied);
  assertRetained(tracker.records, tracker.objects[4].identity, true);

  // Continue to offer the other records. Once they are all in flight there
  // is no packet merely because the history scanner has wrapped around.
  for (unsigned i = 2; i <= 4; ++i) {
    waitForTx(i * 60000U + 1000U);
    assert(live_sequence == i);
    uplink = {};
  }
  for (uint32_t now = 301000U; now < 421000U; now += 1000U) {
    tracker.records.poll();
    assert(sender.update(now, true) != Sender::Result::kSent);
  }
  assert(seals == before + 5);
  // #1 timed out. Retransmission reuses the committed ciphertext and nonce.
  assert(tracker.records.findCachedExact(tracker.objects[0].identity, exact) ==
         ObservationStore::ControlLookupResult::kFound);
  waitForTx(421000U);
  assert(seals == before + 5 && uplink.size == exact.object_size);
  assert(!memcmp(uplink.payload, exact.object, uplink.size));
  uplink = {};
}

// A fresh report arrives while the previous one awaits its grouped receipt.
// Exercise the same newest-first sender and Gateway ingress the lab firmware uses.
void testFirstUplinkIsVisibleAndReceiptable() {
  using namespace orun_tlp;
  Fixture tracker;
  tracker.admitGrant();
  Grant grant;
  ProductFlash gw_flash;
  ProductCustodyStore store(gw_flash, kProductPages);
  assert(store.begin());
  GatewayLocalCustodyOwner gateway(store);
  GatewayCustodyAuthorizedIngress ingress(gateway, grant, kGateA);
  RadioManager gw_radio, collar;
  collar_radio = &collar;
  GatewayProductCustodyRuntime gateway_loop(gw_radio, ingress);
  ProductSecureCrypto crypto(tracker.security);
  using Sender = TrackerCriticalEventTx;
  Sender sender(tracker.records, crypto, collar, Sender::Mode::kPeriodicHistory);
  using Event = GatewayProductCustodyRuntime::Event;
  simulation_ms = 10;
  downlinks = 0;
  uplink = {};
  auto receivePosition = [&](uint32_t expected) {
    for (unsigned n = 0; n < 1000 && !uplink.size; ++n) {
      tracker.records.poll();
      sender.update(simulation_ms, true);
    }
    assert(uplink.size && uplink.payload[4] == 3 && live_sequence == expected);
    const auto first = uplink;
    assert(gateway_loop.update(true) == Event::kCustodyStarted);
    bool visible = false;
    for (unsigned n = 0; n < 100 && !visible; ++n)
      visible = gateway_loop.update(true) == Event::kLiveDurable;
    assert(visible);
    ProductCustodyStore::Handle handle{};
    assert(store.requestCustody(first.payload, first.size, &handle) ==
           ProductCustodyStore::Admission::kDuplicateHeld);
    uint8_t saved[custody_store_product_format::kMaxObjectSize]{};
    size_t size = 0;
    assert(store.readConfirmedHeld(handle, saved, size) == ProductCustodyStore::Lookup::kFound);
    assert(size == first.size && !memcmp(saved, first.payload, size));
  };
  receivePosition(5);
  assert(downlinks == 0);
  simulation_ms += 1001;
  assert(gateway_loop.update(true) == Event::kAckTxStarted && downlinks == 1);
  // Drop that receipt. A newly appended fix does not wait for its timeout.
  const auto newer = append(tracker.records);
  simulation_ms += 15000;
  receivePosition(newer.sequence);
  assert(downlinks == 1);
  // Next isolated report occurs after an hour. The Gateway can return the
  // partial group inside this receive window without a repeated position.
  simulation_ms += 3600000U;
  const auto latest = append(tracker.records);
  receivePosition(latest.sequence);
  simulation_ms += 1001;
  assert(gateway_loop.update(true) == Event::kAckTxStarted);
  ReleaseOwner release(tracker.security, tracker.records);
  assert(release.submit(receipt, receipt_size) == ReleaseOwner::Submit::kStarted);
  while (release.pending()) {
    tracker.records.poll();
    assert(release.service() != ReleaseOwner::Service::kFailed);
  }
  assertRetained(tracker.records, latest, true);
  assertRetained(tracker.records, tracker.objects[4].identity, false);
  puts("First position immediately available at Gateway; first uplink accepted by Tracker receipt: PASS");
}

void testInterleavedCollarsReceiveSeparateReceipts() {
  using namespace orun_tlp;
  constexpr uint64_t second = kDevice + 1U;
  struct TwoGrants final : GatewayCustodyGrantProvider {
    bool activeGrant(uint64_t id, uint32_t epoch, GatewayCustodyAckScope& scope,
                     uint8_t (&key)[32]) override {
      scope = allowedScope();
      if ((id != kDevice && id != second) || epoch != scope.key_epoch) return false;
      scope.tracker_device_id = id;
      fromHex32(key);
      if (id == second) key[0] ^= 1U;
      return true;
    }
  } grants;
  ProductFlash flash;
  ProductCustodyStore store(flash, kProductPages);
  assert(store.begin());
  GatewayLocalCustodyOwner owner(store);
  GatewayCustodyAuthorizedIngress ingress(owner, grants, kGateA);
  RadioManager radio;
  collar_radio = nullptr;
  GatewayProductCustodyRuntime runtime(radio, ingress);
  GatewayProductLiveIngress direct(store, grants, kGateA);
  runtime.attachLiveIngress(direct);
  using Event = GatewayProductCustodyRuntime::Event;
  const auto offer = [&](uint64_t id, uint32_t counter, uint8_t flags = 3U) {
    const auto original = makeObject({});
    tlp::ProductSecureFrame frame{};
    assert(tlp::decodeProductSecureFrame(original.object, original.object_size, &frame)
           == tlp::ProductSecureCodecResult::kOk);
    frame.path_flags = flags;
    frame.device_id = id;
    frame.security_counter = counter;
    assert(tlp::encodeProductSecureFrame(frame, uplink.payload, sizeof(uplink.payload))
           == tlp::ProductSecureCodecResult::kOk);
    uplink.size = original.object_size;
    return runtime.update(true);
  };
  simulation_ms = 1000;
  downlinks = 0;
  for (uint64_t id : {kDevice, second}) {
    assert(offer(id, 42) == Event::kCustodyStarted);
    bool durable = false;
    for (unsigned n = 0; n < 150 && !durable; ++n)
      durable = runtime.update(true) == Event::kLiveDurable;
    assert(durable);
  }
  assert(offer(second + 1, 42) == Event::kDenied);
  // The first collar has filled its exact cache and sends direct. Its
  // receive window can clear only its own older held packet.
  simulation_ms += 10;
  assert(offer(kDevice, 43, 0) == Event::kCustodyStarted);
  for (unsigned n = 0; n < 150 && direct.pending(); ++n) runtime.update(true);
  assert(!direct.pending());
  assert(runtime.update(true) == Event::kAckTxStarted);
  ack::Header header{};
  assert(ack::decode(receipt, receipt_size, header) && header.count == 1 &&
         header.tracker_device_id == kDevice);
  assert(runtime.reservedPages() != 0);
  simulation_ms += 1001;
  assert(runtime.update(true) == Event::kAckTxStarted);
  assert(ack::decode(receipt, receipt_size, header) && header.count == 1 &&
         header.tracker_device_id == second);
  assert(downlinks == 2 && runtime.reservedPages() == 0);
  puts("Two enrolled collars, same epoch and counter, separate receipts; unknown collar denied: PASS");
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


void testReservedLivePriorityBeforeBacklog() {
  using namespace orun_tlp;
  Fixture tracker;
  tracker.admitGrant();
  RadioManager collar;
  collar_radio = &collar;
  ProductSecureCrypto crypto(tracker.security);
  using Sender = TrackerCriticalEventTx;
  Sender sender(tracker.records, crypto, collar,
                Sender::Mode::kPeriodicHistory);

  auto pollUntilSent = [&](uint32_t now, bool allow_old) {
    simulation_ms = now;
    for (unsigned i = 0; i < 2000; ++i) {
      tracker.records.poll();
      const auto result = sender.update(now, true, allow_old);
      assert(result != Sender::Result::kBlocked);
      assert(result != Sender::Result::kReadError);
      if (result == Sender::Result::kSent) return true;
    }
    return false;
  };

  uplink = {};
  assert(pollUntilSent(1000U, false));
  assert(live_sequence == 5U);
  assert(uplink.size == tlp::kPeriodicFrameSize);
  assert(uplink.payload[4] == 3U);
  uplink = {};

  // Old history must not consume the reserved live opportunity.
  assert(!pollUntilSent(61000U, false));
  assert(!uplink.size);

  // A new report must bypass the waiting older records.
  const auto latest = append(tracker.records);
  assert(pollUntilSent(62000U, false));
  assert(live_sequence == latest.sequence);
  assert(uplink.payload[4] == 3U);
  uplink = {};

  assert(!pollUntilSent(123000U, false));
  assert(!uplink.size);

  // Surplus budget restores normal history transmission.
  assert(pollUntilSent(124000U, true));
  assert(live_sequence == 1U);
  uplink = {};

  puts("LIVE PRIORITY: fresh before old, reserved budget, "
       "backlog resumes: PASS");
}


void testOrun302Cadence(uint32_t interval) {
  using namespace orun_tlp;

  Fixture tracker(3);
  tracker.admitGrant();

  RadioManager collar;
  collar_radio = &collar;

  ProductSecureCrypto crypto(tracker.security);

  using Sender = TrackerCriticalEventTx;

  Sender sender(
      tracker.records, crypto, collar,
      Sender::Mode::kPeriodicHistory);

  ReleaseOwner release(tracker.security, tracker.records);

  RfDutyBudget budget;

  const uint8_t size = tlp::kPeriodicFrameSize;

  const uint32_t cost = RfDutyBudget::costMs(size);

  const uint32_t gap =
      cost + radio_config::kWindowedRxAfterTxMs;

  const uint32_t start = 1000U;

  uint32_t next_live = start;
  uint32_t newest = 0U;
  uint32_t last_tx = 0U;

  unsigned live_count = 0U;
  unsigned history_count = 0U;
  unsigned generated = 0U;

  bool seen[8]{};

  uplink = {};
  radio_busy = false;

  for (uint32_t now = start;
       now < start + 4U * interval;
       now += 1000U) {

    simulation_ms = now;

    if (now == next_live) {
      newest = append(tracker.records).sequence;
      ++generated;
      next_live += interval;
    }

    const uint32_t remaining = next_live - now;

    const bool eligible =
        (last_tx == 0U || now - last_tx >= gap) &&
        budget.available(now, size);

    const bool allow_old =
        eligible && remaining > gap + 200U &&
        budget.availableBeforeLive(now, size,
                                   remaining, size);

    for (unsigned poll = 0; poll < 2000U; ++poll) {
      tracker.records.poll();

      const auto result =
          sender.update(now, eligible, allow_old);

      assert(result != Sender::Result::kBlocked);
      assert(result != Sender::Result::kReadError);

      if (result != Sender::Result::kSent)
        continue;

      assert(eligible);

      assert(uplink.size == size);
      assert(uplink.payload[4] == 3U);

      assert(budget.reserve(now, size));
      budget.finish(now + cost);

      if (last_tx != 0U)
        assert(now - last_tx >= gap);

      last_tx = now;

      const auto id = sender.selectedIdentity();

      assert(id.valid() && id.sequence <= 7U);
      assert(!seen[id.sequence]);

      seen[id.sequence] = true;

      if (id.sequence == newest) {
        // No fresh report waits behind old records.
        assert(now == next_live - interval);
        ++live_count;
      } else {
        assert(id.sequence <= 3U);
        assert(allow_old);
        ++history_count;
      }

      osc::ExactObject exact{};

      assert(tracker.records.findCachedExact(id, exact) ==
             ObservationStore::ControlLookupResult::kFound);

      assert(exact.object_size == uplink.size);

      assert(!memcmp(exact.object, uplink.payload,
                     uplink.size));

      uint8_t wire[ack::kMaxSize]{};
      size_t length = 0U;

      buildAck(&exact, 1U, wire, length);

      assert(release.submit(wire, length) ==
             ReleaseOwner::Submit::kStarted);

      assert(release.service() ==
             ReleaseOwner::Service::kWaitingFlash);

      assert(nextDurableOutcome(
                 release, tracker.records) ==
             ReleaseOwner::Service::kReleasedOne);

      assert(release.service() ==
             ReleaseOwner::Service::kApplied);

      assertRetained(tracker.records, id, true);

      uplink = {};
      break;
    }

    if (generated == 4U &&
        live_count == 4U &&
        history_count == 3U)
      break;
  }

  assert(generated == 4U);
  assert(live_count == 4U);
  assert(history_count == 3U);

  printf("ORUN-302 interval=%lu live=%u history=%u "
         "authenticated releases=7 PASS\n",
         (unsigned long)interval,
         live_count, history_count);

  collar_radio = nullptr;
}


void testOrun302FifoRetirement(bool cache_ready) {
  using namespace orun_tlp;

  Fixture tracker(1);
  tracker.admitGrant();

  RadioManager collar;
  collar_radio = &collar;

  ProductSecureCrypto crypto(tracker.security);

  using Sender = TrackerCriticalEventTx;

  Sender sender(tracker.records, crypto, collar,
                Sender::Mode::kPeriodicHistory);

  uplink = {};
  radio_busy = !cache_ready;

  const auto retired = tracker.objects[0].identity;

  if (cache_ready) {
    bool durable = false;

    for (unsigned i = 0; i < 2000U; ++i) {
      tracker.records.poll();

      const auto result = sender.update(1000U, true);

      assert(result != Sender::Result::kBlocked);
      assert(result != Sender::Result::kReadError);
      assert(result != Sender::Result::kSent);

      if (result == Sender::Result::kCacheDurable) {
        durable = true;
        break;
      }
    }

    assert(durable);
  } else {
    sender.update(1000U, true);
  }

  assert(sender.selectedIdentity().sequence ==
         retired.sequence);

  // Fill the actual 32-page product region and rotate it.
  constexpr uint32_t capacity =
      (kProductPages - osf::kControlPageCount) *
      osf::kDataRecordsPerPage;

  static_assert(capacity == 1260U, "Product geometry changed");

  osf::RecordIdentity latest{};

  for (uint32_t sequence = 2U;
       sequence <= capacity + 1U;
       ++sequence) {
    latest = append(tracker.records);
    assert(latest.sequence == sequence);
  }

  ObservationStore::Record missing{};

  assert(tracker.records.store().lookup(retired, missing) ==
         ObservationStore::LookupResult::kNone);

  assert(tracker.records.store().diagnostics()
             .capacity_lost_total == 42U);

  assert(tracker.records.diagnostics()
             .custody_release_committed == 0U);

  // A retired candidate is not a corrupt record or a delivered record.
  if (cache_ready) {
    const auto result =
        sender.update(2000U, false, true);

    assert(result != Sender::Result::kBlocked);
    assert(!sender.blocked());
  }

  radio_busy = false;

  bool sent = false;

  for (unsigned i = 0; i < 2000U; ++i) {
    tracker.records.poll();

    const auto result =
        sender.update(3000U, true, true);

    assert(result != Sender::Result::kBlocked);
    assert(result != Sender::Result::kReadError);

    if (result == Sender::Result::kSent) {
      sent = true;
      break;
    }
  }

  assert(sent);
  assert(live_sequence == latest.sequence);

  assert(tracker.records.diagnostics()
             .custody_release_committed == 0U);

  assertRetained(tracker.records, latest, false);

  uplink = {};
  collar_radio = nullptr;

  printf("ORUN-302 FIFO capacity=1260 lost=42 "
         "newest=1261 cache_ready=%u PASS\n",
         unsigned(cache_ready));
}


void testOrun302LostAckCadence(uint32_t interval) {
  using namespace orun_tlp;

  Fixture tracker(3);
  tracker.admitGrant();

  RadioManager collar;
  collar_radio = &collar;

  ProductSecureCrypto crypto(tracker.security);

  using Sender = TrackerCriticalEventTx;

  Sender sender(tracker.records, crypto, collar,
                Sender::Mode::kPeriodicHistory);

  RfDutyBudget budget;

  const uint8_t size = tlp::kPeriodicFrameSize;
  const uint32_t cost = RfDutyBudget::costMs(size);

  const uint32_t gap =
      cost + radio_config::kWindowedRxAfterTxMs;

  uint32_t last_tx = 0U;
  unsigned live = 0U;
  unsigned custody = 0U;
  unsigned without_custody = 0U;

  osc::ExactObject first_exact{};

  uplink = {};
  radio_busy = false;

  auto transmit = [&](uint32_t now, bool allow_old) {
    simulation_ms = now;

    assert(last_tx == 0U || now - last_tx >= gap);
    assert(budget.available(now, size));

    bool sent = false;

    for (unsigned i = 0; i < 2000U; ++i) {
      tracker.records.poll();

      const auto result =
          sender.update(now, true, allow_old);

      assert(result != Sender::Result::kBlocked);
      assert(result != Sender::Result::kReadError);

      if (result == Sender::Result::kSent) {
        sent = true;
        break;
      }
    }

    assert(sent);
    assert(uplink.size == size);

    assert(budget.reserve(now, size));
    budget.finish(now + cost);

    last_tx = now;
  };

  // Twelve fresh reports without a single received ACK.
  // Backlog admission is deliberately closed except for
  // the exact-retry opportunity below.
  for (uint32_t i = 0U; i < 12U; ++i) {
    const uint32_t now = 1000U + i * interval;

    const auto newest = append(tracker.records);

    transmit(now, false);

    assert(live_sequence == newest.sequence);

    ++live;

    if (i < Sender::kHistoryCacheCeiling) {
      assert(uplink.payload[4] == 3U);

      ++custody;

      if (i == 0U) {
        assert(tracker.records.findCachedExact(
                   newest, first_exact) ==
               ObservationStore::ControlLookupResult::kFound);
      }
    } else {
      // Full exact cache must not stop current visibility.
      // No custody promise is made for this uplink.
      assert(uplink.payload[4] == 0U);

      ++without_custody;
    }

    assertRetained(tracker.records, newest, false);

    assert(tracker.records.diagnostics()
               .custody_release_committed == 0U);

    uplink = {};

    if (i == 6U) {
      // Allow a history retry after the first exact
      // object's receipt wait has expired.
      const uint32_t retry_at = now + 15000U;

      assert(retry_at - 1000U >= Sender::kReceiptWaitMs);

      const unsigned before = seals;

      transmit(retry_at, true);

      assert(seals == before);

      assert(uplink.payload[4] == 3U);

      osc::ExactObject retry{};

      assert(tracker.records.findCachedExact(
                 sender.selectedIdentity(), retry) ==
             ObservationStore::ControlLookupResult::kFound);

      assert(retry.object_size == uplink.size);

      assert(!memcmp(retry.object, uplink.payload,
                     uplink.size));

      assertRetained(
          tracker.records, retry.identity, false);

      uplink = {};
    }
  }

  assert(live == 12U);
  assert(custody == 4U);
  assert(without_custody == 8U);

  uint16_t occupied = 0U;

  assert(tracker.records.store().exactObjectCount(occupied));
  assert(occupied == 4U);

  assert(tracker.records.diagnostics()
             .custody_release_committed == 0U);

  // One authenticated receipt finally arrives.
  uint8_t wire[ack::kMaxSize]{};
  size_t length = 0U;

  buildAck(&first_exact, 1U, wire, length);

  ReleaseOwner release(
      tracker.security, tracker.records);

  assert(release.submit(wire, length) ==
         ReleaseOwner::Submit::kStarted);

  assert(release.service() ==
         ReleaseOwner::Service::kWaitingFlash);

  assert(nextDurableOutcome(
             release, tracker.records) ==
         ReleaseOwner::Service::kReleasedOne);

  assert(release.service() ==
         ReleaseOwner::Service::kApplied);

  assertRetained(
      tracker.records, first_exact.identity, true);

  assert(tracker.records.store().exactObjectCount(occupied));
  assert(occupied == 3U);

  // New live record can request custody again.
  const auto recovered = append(tracker.records);

  transmit(1000U + 12U * interval, false);

  assert(live_sequence == recovered.sequence);
  assert(uplink.payload[4] == 3U);

  assertRetained(tracker.records, recovered, false);

  assert(tracker.records.diagnostics()
             .custody_release_committed == 1U);

  uplink = {};
  collar_radio = nullptr;

  printf("ORUN-302 LOST ACK interval=%lu "
         "live=12 custody=4 no-custody=8 "
         "exact-retry=PASS recovery=PASS\n",
         (unsigned long)interval);
}


void testOrun303BudgetPressure(uint8_t count) {
  using namespace orun_tlp;

  assert(count >= 2U && count <= 5U);

  Fixture tracker;
  tracker.admitGrant();
  tracker.cacheUpTo(count);

  Grant grant;

  ProductFlash flash;
  ProductCustodyStore store(flash, kProductPages);

  assert(store.begin());

  GatewayLocalCustodyOwner owner(store);

  GatewayCustodyAuthorizedIngress ingress(
      owner, grant, kGateA);

  RadioManager gateway;

  GatewayProductCustodyRuntime runtime(
      gateway, ingress);

  collar_radio = nullptr;
  uplink = {};
  downlinks = 0U;
  prepare_attempts = 0U;
  prepare_delay_ms = 0U;
  defer_starts = 0U;
  stale_slot = -1;
  radio_busy = false;

  // Consume most of the actual rolling RF ledger.
  // 161 x 2217 ms = 356937 ms.
  // Remaining credit = 3063 ms.
  for (uint32_t i = 0U; i < 161U; ++i) {
    simulation_ms = 1000U + i * 3000U;

    RadioBudgetHostProbe::consume(
        gateway, simulation_ms, 100U);
  }

  const uint32_t first = 500000U;

  simulation_ms = first;

  assert(gateway.rfAirtimeUsedMs() ==
         161U * RfDutyBudget::costMs(100U));

  assert(gateway.gatewayAirtimeAvailable(56U));

  // An individual ACK fits physically, but sending it
  // would spend the reserve for a later group.
  assert(!gateway.gatewayAirtimeAvailable(
      56U, 2000000U));

  const uint8_t group_size =
      ack::kHeaderSize +
      count * ack::kRefSize +
      ack::kTagSize;

  assert(gateway.gatewayAirtimeAvailable(group_size));

  auto offer = [&](unsigned index) {
    const auto& object = tracker.objects[index];

    assert(gateway.sendProductSecurePacket(
        object.object, object.object_size));

    for (unsigned i = 0U; i < 200U; ++i)
      runtime.update(true);

    assert(!uplink.size);
  };

  // Real durable Gateway admission. One report per minute.
  for (unsigned i = 0U; i < count; ++i) {
    simulation_ms = first + i * 60000U;

    // For the five-reference case, the radio remains
    // unavailable at the four-reference threshold.
    radio_busy = count == 5U && i >= 3U;

    offer(i);

    if (i < 3U) {
      assert(downlinks == 0U);

      simulation_ms += 1001U;

      runtime.update(true);

      assert(downlinks == 0U);
      assert(runtime.reservedPages() != 0U);
    }
  }

  if (count < 4U) {
    radio_busy = false;

    // The group is now old enough, but the previous
    // collar RX window is closed.
    simulation_ms = first + 180001U;

    for (unsigned i = 0; i < 100U; ++i)
      runtime.update(true);

    assert(downlinks == 0U);

    // A byte-identical retry opens a new RX opportunity.
    // It must not add a duplicate ACK reference.
    offer(0U);
  }

  if (count == 5U) {
    assert(downlinks == 0U);
    radio_busy = false;
  }

  for (unsigned i = 0U;
       i < 200U && !downlinks; ++i)
    runtime.update(true);

  assert(downlinks == 1U);

  ack::Header header{};

  assert(ack::decode(
      receipt, receipt_size, header));

  assert(header.count == count);
  assert(header.tracker_device_id == kDevice);
  assert(receipt_size == group_size);

  assert(runtime.reservedPages() == 0U);

  assert(gateway.rfAirtimeUsedMs() ==
         161U * RfDutyBudget::costMs(100U) +
         RfDutyBudget::costMs(group_size));

  assert(gateway.rfAirtimeUsedMs() <=
         RfDutyBudget::kMaxMs);

  // Verify the real HMAC on the Tracker and commit
  // each independent durable release marker.
  ReleaseOwner release(
      tracker.security, tracker.records);

  assert(release.submit(receipt, receipt_size) ==
         ReleaseOwner::Submit::kStarted);

  for (unsigned i = 0U; i < count; ++i) {
    assert(release.service() ==
           ReleaseOwner::Service::kWaitingFlash);

    assert(nextDurableOutcome(
               release, tracker.records) ==
           ReleaseOwner::Service::kReleasedOne);
  }

  assert(release.service() ==
         ReleaseOwner::Service::kApplied);

  for (unsigned i = 0U; i < 5U; ++i)
    assertRetained(
        tracker.records,
        tracker.objects[i].identity,
        i < count);

  printf("ORUN-303 RF PRESSURE count=%u "
         "ack=%lu bytes released=%u "
         "budget=%lu PASS\n",
         unsigned(count),
         (unsigned long)receipt_size,
         unsigned(count),
         (unsigned long)gateway.rfAirtimeUsedMs());

  uplink = {};
}

} // namespace

#ifndef ORUN_KQ05_TRACKER_BATCH_RADIO_RUNTIME_HOST_TEST
int main() {
  // Run the inherited real SecurityStore full-digest/HMAC regressions too.
  // They are intentionally included in this translation unit; do not
  // suppress -Werror or leave security tests compiled but unused.
  testOneStaleHandleDoesNotPoisonItsPeer();
  testLongOutageUsesEveryExactSlot();
  testReservedEventSlotAndNoFix();
  testFailedWindowsRecover(4);
  testFailedWindowsRecover(5);
  reportWindowTrace();
  downlinks = simulation_ms = 0;
  testGatewayToTrackerWithMissingRecord();
  testOneTransmissionForFreshAndHistory();
  testReservedLivePriorityBeforeBacklog();
  testOrun302Cadence(60000U);
  testOrun302Cadence(900000U);
  testOrun302FifoRetirement(false);
  testOrun302FifoRetirement(true);
  testOrun302LostAckCadence(60000U);
  testOrun302LostAckCadence(900000U);
  testFirstUplinkIsVisibleAndReceiptable();
  testInterleavedCollarsReceiveSeparateReceipts();
  testActualRootDerivedFiveAndSubsets();
  testTamperAndRevocationDoNotPublishCapability();
  testFiveExactIndependentDurableMarkers();
  testNoGrantTamperedOrIncompleteGroupNeverStartsFlash();
  testFailureOnSecondMarkerNeverReleasesLaterRecords();
  testRevocationBetweenMarkersStopsRemaining();
  testCrashAfterOneMarkerRequiresNewRemainingSubsetAck();
  for (uint8_t count = 2U; count <= 5U; ++count)
    testOrun303BudgetPressure(count);
  puts("KQ05 Tracker 1..5 REAL V3 HMAC -> selective durable OBS1 marker, failure/reboot: PASS (HOST ONLY)");
  puts("NO LORA TX/RX, NO REAL FLASH, NO WHOLE REGION ERASE, NO DEVICE UPLOAD");
  return 0;
}
#endif  // ORUN_KQ05_TRACKER_BATCH_RADIO_RUNTIME_HOST_TEST
