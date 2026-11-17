// KQ05 critical EVENT/legacy custody bridge, PC-only fake NOR + fake sealer.
// The legacy pilot and its existing fake radio supply test-only test doubles.
// This is NOT live RF, real crypto or field-release evidence.
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "tracker_critical_event_tx.h"

#define main sf5k3v_previous_main
#include "test_sf5k3v_tracker_pilot.cpp"
#undef main

namespace orun_tlp {
ProductSecureCryptoResult ProductSecureCrypto::protectNextEvent(
    uint8_t flags, const tlp::EventPlaintext& plaintext,
    uint8_t (&out)[tlp::kEventFrameSize]) {
  ++g_seals;
  tlp::ProductSecureFrame frame{};
  frame.app_family = tlp::kProductAppFamilyEvent;
  frame.path_flags = flags;
  frame.device_id = kDevice;
  frame.key_epoch = 7U;
  frame.security_counter = g_seals;
  frame.ciphertext_len = tlp::kEventPlaintextSize;
  for (size_t i = 0U; i < frame.ciphertext_len; ++i)
    frame.ciphertext[i] = static_cast<uint8_t>(
        i ^ static_cast<uint8_t>(plaintext.record_sequence));
  return tlp::encodeProductSecureFrame(frame, out, sizeof(out)) ==
                 tlp::ProductSecureCodecResult::kOk
             ? ProductSecureCryptoResult::kOk
             : ProductSecureCryptoResult::kEngineError;
}
}  // namespace orun_tlp

namespace {
using EventSender = orun_tlp::TrackerCriticalEventTx;
void durableCache(ProductRecordStore& records, EventSender& sender,
                  uint32_t clock_ms) {
  bool done = false;
  for (unsigned i = 0U; i < 20000U; ++i) {
    records.poll();
    const auto r = sender.update(clock_ms, true);
    if (r == EventSender::Result::kCacheDurable) {
      done = true;
      break;
    }
    assert(r != EventSender::Result::kSent &&
           r != EventSender::Result::kBlocked);
  }
  assert(done);
}
void eventCustodyRetryAndRelease() {
  resetFakes();
  FakeFlash flash;
  FixedIncarnation source;
  ProductRecordStore records(flash, kPages, &source);
  records.begin(kDevice, kTracker);
  assert(records.mounted());
  assert(store(records, 1U).stored);
  appendDurableEvent(records);  // EVENT seq=2, same OBS1 allocator.

  orun_tlp::SecurityStore security(flash, flash);
  orun_tlp::ProductSecureCrypto crypto(security);
  orun_tlp::RadioManager radio;
  EventSender events(records, crypto, radio);
  orun_tlp::TrackerAcklessLiveTx live(records, crypto, radio);

  assert(events.update(1000U, true) ==
         EventSender::Result::kCachePending);
  assert(g_seals == 1U && g_sends == 0U);
  assert(events.update(1001U, true) == EventSender::Result::kNone);
  durableCache(records, events, 1002U);
  assert(g_sends == 0U);
  uint16_t count = 99U;
  assert(records.store().exactObjectCount(count) && count == 1U);

  g_deny_tx = true;
  assert(events.update(1003U, true) == EventSender::Result::kDeferred);
  assert(g_sends == 0U && g_seals == 1U);
  assert(events.update(1004U, true) == EventSender::Result::kNone);
  g_deny_tx = false;
  assert(events.update(31003U, true) == EventSender::Result::kSent);
  assert(events.waitingAck() && events.selectedIdentity().sequence == 2U);
  assert(g_last_size == orun_tlp::tlp::kEventFrameSize && g_seals == 1U);
  orun_tlp::tlp::ProductSecureFrame packet{};
  assert(orun_tlp::tlp::decodeProductSecureFrame(g_last,g_last_size,&packet) ==
         orun_tlp::tlp::ProductSecureCodecResult::kOk);
  assert(packet.app_family == orun_tlp::tlp::kProductAppFamilyEvent);
  assert((packet.path_flags &
          orun_tlp::tlp::kProductPathFlagCustodyRequested) != 0U);
  uint8_t saved[orun_tlp::tlp::kEventFrameSize]{};
  memcpy(saved, g_last, sizeof(saved));
  const unsigned writes_after_cache = flash.program_calls;
  assert(events.update(31004U, true) == EventSender::Result::kNone);

  // The EVENT's ACK wait does NOT gate routine LIVE. EVENT tail is ignored
  // by the newest-PERIODIC selector, but remains durably retained.
  assert(live.update(31005U, true) ==
         orun_tlp::TrackerAcklessLiveTx::Event::kSent);
  assert(live.lastSent().sequence == 1U);
  assert(g_last_size == orun_tlp::tlp::kPeriodicFrameSize);
  assert(g_seals == 2U);
  assert(flash.program_calls == writes_after_cache);

  // Additional critical occurrences remain on OBS1, but the unconfirmed
  // earlier event retains ownership of the single critical custody flight.
  appendDurableEvent(records);  // EVENT seq=3
  assert(events.update(32000U, true) == EventSender::Result::kNone);
  assert(events.selectedIdentity().sequence == 2U);

  assert(events.update(91003U, true) == EventSender::Result::kSent);
  assert(g_last_size == sizeof(saved));
  assert(memcmp(g_last,saved,sizeof(saved)) == 0);
  assert(g_seals == 2U);  // retry exact ciphertext; no nonce reuse/reseal
  const osf::RecordIdentity confirmed = events.selectedIdentity();
  ObservationStore::Record before{};
  assert(records.store().lookup(confirmed,before) ==
         ObservationStore::LookupResult::kFound && !before.released);

  // Simulate the EXISTING custody owner having verified authenticated ACK
  // and durably committed a release marker. This test does not forge a MAC.
  releaseAndReboot(flash, source, confirmed);
  ProductRecordStore reboot(flash, kPages, &source);
  reboot.begin(kDevice, kTracker);
  assert(reboot.mounted());
  EventSender recovered(reboot, crypto, radio);
  // Next retained EVENT (seq=3) can move only after seq=2 was released.
  assert(recovered.update(93000U, true) ==
         EventSender::Result::kCachePending);
  assert(recovered.selectedIdentity().sequence == 3U);
  assert(recovered.update(93001U, true) == EventSender::Result::kNone);
  durableCache(reboot,recovered,93002U);
  assert(recovered.update(93003U, true) == EventSender::Result::kSent);
  assert(recovered.selectedIdentity().sequence == 3U);
}
void noAckNeverReleasesEvent() {
  resetFakes();
  FakeFlash flash;
  FixedIncarnation source;
  ProductRecordStore records(flash, kPages, &source);
  records.begin(kDevice, kTracker);
  assert(store(records,1U).stored);
  appendDurableEvent(records);
  orun_tlp::SecurityStore security(flash,flash);
  orun_tlp::ProductSecureCrypto crypto(security);
  orun_tlp::RadioManager radio;
  EventSender sender(records,crypto,radio);
  // First scheduling after 2^31 ms of uptime must not be suppressed by
  // an initially zero (unarmed) deadline.
  assert(sender.update(0x90000000U,true) ==
         EventSender::Result::kCachePending);
  durableCache(records,sender,0x90000002U);
  assert(sender.update(0x90000003U,true) == EventSender::Result::kSent);
  assert(sender.update(0x90000004U,false) == EventSender::Result::kNone);
  ObservationStore::Record retained{};
  assert(records.store().lookup(sender.selectedIdentity(),retained) ==
         ObservationStore::LookupResult::kFound);
  assert(!retained.released);
}
}  // namespace

int main() {
  eventCustodyRetryAndRelease();
  noAckNeverReleasesEvent();
  puts("KQ05 critical EVENT exact-cache / existing custody ACK / LIVE bypass: PASS (PC fake NOR)");
}
