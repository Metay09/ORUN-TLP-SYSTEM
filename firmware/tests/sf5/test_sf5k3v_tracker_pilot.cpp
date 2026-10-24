// SF5K3V: actual ProductRecordStore/ObservationStore fake NOR and delivery
// state machine. The following fake sealer is NOT an AEAD crypto test; actual
// ProductSecureCrypto is independently tested by its RAK4630 target probe.
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "security_store.h"
#include "tracker_product_tx_pilot.h"

#define main sf5f_original_host_main
#include "test_sf5f_product_record_store.cpp"
#undef main

namespace {
unsigned g_seals = 0U;
unsigned g_sends = 0U;
bool g_radio_ready = true;
uint8_t g_last[orun_tlp::tlp::kPeriodicFrameSize]{};
size_t g_last_size = 0U;
void resetFakes() {
  g_seals = 0U;
  g_sends = 0U;
  g_radio_ready = true;
  g_last_size = 0U;
  memset(g_last, 0, sizeof(g_last));
}
}  // namespace

namespace orun_tlp {
// Test-only deterministic structural wire sealer, with NO secret keys.
ProductSecureCryptoResult ProductSecureCrypto::protectNextPeriodic(
    uint8_t flags, const tlp::PeriodicObservationPlaintext& plaintext,
    uint8_t (&out)[tlp::kPeriodicFrameSize]) {
  ++g_seals;
  tlp::ProductSecureFrame frame{};
  frame.app_family = tlp::kProductAppFamilyPeriodic;
  frame.path_flags = flags;
  frame.device_id = kDevice;
  frame.key_epoch = 7U;
  frame.security_counter = g_seals;
  frame.ciphertext_len = tlp::kPeriodicPlaintextSize;
  for (size_t i = 0; i < frame.ciphertext_len; ++i)
    frame.ciphertext[i] = static_cast<uint8_t>(
        i ^ static_cast<uint8_t>(plaintext.record_sequence));
  return tlp::encodeProductSecureFrame(frame, out, sizeof(out)) ==
                 tlp::ProductSecureCodecResult::kOk
             ? ProductSecureCryptoResult::kOk
             : ProductSecureCryptoResult::kEngineError;
}
bool RadioManager::canSend() const { return g_radio_ready; }
uint64_t RadioManager::deviceId() const { return kDevice; }
bool RadioManager::sendProductSecurePacket(const uint8_t* object, size_t n) {
  if (!g_radio_ready || n != sizeof(g_last)) return false;
  memcpy(g_last, object, n);
  g_last_size = n;
  ++g_sends;
  return true;
}
}  // namespace orun_tlp

namespace {
using Pilot = orun_tlp::TrackerProductTxPilot;
void driveCache(ProductRecordStore& records, Pilot& pilot) {
  bool ready = false;
  for (unsigned i = 0; i < 10000U; ++i) {
    records.poll();
    const auto event = pilot.update(false);
    if (event == Pilot::Event::kCacheDurable) {
      ready = true;
      break;
    }
    assert(event != Pilot::Event::kSent &&
           event != Pilot::Event::kBlocked);
  }
  assert(ready && "cache must be durable before RF");
}
void successAndReboot(bool async) {
  resetFakes();
  FakeFlash flash;
  FixedIncarnation source;
  ProductRecordStore records(flash, kPages, &source);
  records.begin(kDevice, kTracker);
  assert(records.mounted());
  assert(store(records, 1).stored);
  flash.async = async;
  orun_tlp::SecurityStore security(flash, flash);
  orun_tlp::ProductSecureCrypto crypto(security);
  orun_tlp::RadioManager radio;
  Pilot pilot(records, crypto, radio);

  assert(pilot.update(false) == Pilot::Event::kNone);
  assert(g_seals == 0U && g_sends == 0U);
  g_radio_ready = false;
  assert(pilot.update(true) == Pilot::Event::kNone);
  assert(g_seals == 0U && g_sends == 0U);
  g_radio_ready = true;

  assert(pilot.update(true) == Pilot::Event::kCachePending);
  assert(g_seals == 1U && g_sends == 0U);
  assert(pilot.update(true) == Pilot::Event::kNone);
  assert(g_sends == 0U);  // even if fake flash committed synchronously
  driveCache(records, pilot);
  assert(g_sends == 0U);
  assert(pilot.update(true) == Pilot::Event::kSent);
  assert(g_last_size == orun_tlp::tlp::kPeriodicFrameSize && g_sends == 1U);
  assert(pilot.update(true) == Pilot::Event::kNone);
  assert(g_sends == 1U && g_seals == 1U);
  uint8_t first[sizeof(g_last)]{};
  memcpy(first, g_last, sizeof(first));
  const unsigned no_extra_programs = flash.program_calls;

  // SF5L2 rearm: unreleased record -> the SAME cached bytes again, same boot,
  // no new seal (no counter burn) and no flash write.
  assert(pilot.sent() && !pilot.blocked());
  assert(pilot.rearm() && !pilot.sent());
  assert(!pilot.rearm());  // only from the sent state
  assert(pilot.update(true) == Pilot::Event::kSent);
  assert(g_sends == 2U && g_seals == 1U);
  assert(memcmp(first, g_last, sizeof(first)) == 0);
  assert(flash.program_calls == no_extra_programs);
  g_sends = 1U;  // keep the reboot expectations below unchanged
  flash.async = false;  // New boot mounts with synchronous flash recovery.
  ProductRecordStore reboot(flash, kPages, &source);
  reboot.begin(kDevice, kTracker);
  assert(reboot.mounted());
  Pilot next_boot(reboot, crypto, radio);
  assert(next_boot.update(true) == Pilot::Event::kSent);
  assert(g_sends == 2U && g_seals == 1U);
  assert(memcmp(first, g_last, sizeof(first)) == 0);
  assert(flash.program_calls == no_extra_programs);
  ObservationStore::Record retained{};
  assert(reboot.oldestRetained(retained) ==
         ObservationStore::LookupResult::kFound && !retained.released);
}
void cacheFailureKeepsRecord() {
  resetFakes();
  FakeFlash flash;
  FixedIncarnation source;
  ProductRecordStore records(flash, kPages, &source);
  records.begin(kDevice, kTracker);
  assert(store(records, 1).stored);
  // No loss or RF on a failed flash control-page preparation.
  flash.fail_mutation_at = flash.mutation_calls + 1U;
  orun_tlp::SecurityStore security(flash, flash);
  orun_tlp::ProductSecureCrypto crypto(security);
  orun_tlp::RadioManager radio;
  Pilot pilot(records, crypto, radio);
  assert(pilot.update(true) == Pilot::Event::kCachePending);
  bool blocked = false;
  for (unsigned i = 0; i < 10000U; ++i) {
    records.poll();
    if (pilot.update(true) == Pilot::Event::kBlocked) {
      blocked = true;
      break;
    }
  }
  assert(blocked && g_sends == 0U);
  // A failed mutation may quarantine this boot's store. Read on reboot,
  // never bypass its fail-closed state just to satisfy the test.
  flash.fail_mutation_at = 0U;
  ProductRecordStore reboot(flash, kPages, &source);
  reboot.begin(kDevice, kTracker);
  assert(reboot.mounted());
  ObservationStore::Record retained{};
  assert(reboot.oldestRetained(retained) ==
         ObservationStore::LookupResult::kFound && !retained.released);
}
// Drive one live-first selection to a TX; returns the identity sent.
osf::RecordIdentity sendOne(ProductRecordStore& records, Pilot& pilot) {
  for (unsigned i = 0; i < 20000U; ++i) {
    records.poll();
    const auto event = pilot.update(true);
    assert(event != Pilot::Event::kBlocked);
    if (event == Pilot::Event::kSent) {
      const osf::RecordIdentity sent = pilot.lastIdentity();
      assert(pilot.rearm());
      return sent;
    }
  }
  assert(false && "no send");
  return {};
}
// Release one record durably (as an authenticated ACK would), then reboot.
void releaseAndReboot(FakeFlash& flash, FixedIncarnation& source,
                      const osf::RecordIdentity& id) {
  ObservationStore owner(flash, kPages, &source);
  assert(owner.begin(kDevice));
  assert(owner.requestRelease(id));
  for (unsigned n = 0U; n < 1000U && owner.busy(); ++n) owner.poll();
  bool committed = false;
  assert(owner.takeReleaseResult(committed) && committed);
}
// SF5L4: newest first, backlog afterwards; never blocked by a full cache.
void liveFirst() {
  resetFakes();
  FakeFlash flash;
  FixedIncarnation source;
  ProductRecordStore records(flash, kPages, &source);
  records.begin(kDevice, kTracker);
  assert(store(records, 1).stored && store(records, 2).stored &&
         store(records, 3).stored);
  orun_tlp::SecurityStore security(flash, flash);
  orun_tlp::ProductSecureCrypto crypto(security);
  orun_tlp::RadioManager radio;
  Pilot pilot(records, crypto, radio);
  pilot.setLiveFirst(true);
  assert(sendOne(records, pilot).sequence == 3U);  // newest, not oldest
  uint8_t live[sizeof(g_last)]{};
  memcpy(live, g_last, sizeof(live));
  // Unanswered: the SAME live bytes again, no second seal.
  assert(sendOne(records, pilot).sequence == 3U && g_seals == 1U);
  assert(memcmp(live, g_last, sizeof(live)) == 0);
  // A newer record overtakes it immediately.
  assert(store(records, 4).stored);
  assert(sendOne(records, pilot).sequence == 4U && g_seals == 2U);
  assert(sendOne(records, pilot).sequence == 4U);
  // Fill the cache (records 5, 6 -> 4 cached objects: 3,4,5,6).
  assert(store(records, 5).stored);
  assert(sendOne(records, pilot).sequence == 5U);
  assert(store(records, 6).stored);
  assert(sendOne(records, pilot).sequence == 6U && g_seals == 4U);
  // Cache full, newest (7) and oldest (1) uncached: re-send the newest
  // cached object unchanged instead of blocking; nothing is evicted.
  assert(store(records, 7).stored);
  const unsigned programs = flash.program_calls;
  assert(sendOne(records, pilot).sequence == 6U && g_seals == 4U);
  assert(flash.program_calls == programs);

  // Default (SF5K3V semantics) still blocks on a full cache.
  Pilot strict(records, crypto, radio);
  bool blocked = false;
  for (unsigned i = 0; i < 1000U && !blocked; ++i) {
    records.poll();
    blocked = strict.update(true) == Pilot::Event::kBlocked;
  }
  assert(blocked && strict.blocked());

  // The live record is ACKed: a slot frees and the next newest goes first.
  osf::RecordIdentity six{};
  six.incarnation = records.store().incarnation();
  six.sequence = 6U;
  releaseAndReboot(flash, source, six);
  ProductRecordStore reboot(flash, kPages, &source);
  reboot.begin(kDevice, kTracker);
  assert(reboot.mounted());
  Pilot next(reboot, crypto, radio);
  next.setLiveFirst(true);
  assert(sendOne(reboot, next).sequence == 7U && g_seals == 5U);
  // Newest released? Then the backlog drains oldest-first as before.
  osf::RecordIdentity seven = six;
  seven.sequence = 7U;
  releaseAndReboot(flash, source, seven);
  ProductRecordStore later(flash, kPages, &source);
  later.begin(kDevice, kTracker);
  Pilot drain(later, crypto, radio);
  drain.setLiveFirst(true);
  // Newest retained (7) is released; the oldest retained (1) is next, but
  // the cache (3,4,5 + free slot) has room, so 1 is sealed and sent.
  assert(sendOne(later, drain).sequence == 1U);
}
// SF5L4 range walk: newest only; the backlog waits, nothing is skipped.
void liveOnly() {
  resetFakes();
  FakeFlash flash;
  FixedIncarnation source;
  ProductRecordStore records(flash, kPages, &source);
  records.begin(kDevice, kTracker);
  assert(store(records, 1).stored && store(records, 2).stored);
  orun_tlp::SecurityStore security(flash, flash);
  orun_tlp::ProductSecureCrypto crypto(security);
  orun_tlp::RadioManager radio;
  Pilot pilot(records, crypto, radio);
  pilot.setLiveOnly(true);
  assert(sendOne(records, pilot).sequence == 2U);
  for (uint32_t n = 3U; n <= 5U; ++n) {  // cache: 2,3,4,5 (full)
    assert(store(records, n).stored);
    assert(sendOne(records, pilot).sequence == n);
  }
  assert(g_seals == 4U);
  // Out of range: 6 cannot get a slot -> newest cached (5) again, unchanged.
  assert(store(records, 6).stored);
  const unsigned programs = flash.program_calls;
  assert(sendOne(records, pilot).sequence == 5U && g_seals == 4U);
  assert(flash.program_calls == programs);

  osf::RecordIdentity five{};
  five.incarnation = records.store().incarnation();
  five.sequence = 5U;
  releaseAndReboot(flash, source, five);
  ProductRecordStore reboot(flash, kPages, &source);
  reboot.begin(kDevice, kTracker);
  Pilot walk(reboot, crypto, radio);
  walk.setLiveOnly(true);
  assert(sendOne(reboot, walk).sequence == 6U && g_seals == 5U);

  osf::RecordIdentity six = five;
  six.sequence = 6U;
  releaseAndReboot(flash, source, six);
  ProductRecordStore later(flash, kPages, &source);
  later.begin(kDevice, kTracker);
  Pilot idle(later, crypto, radio);
  idle.setLiveOnly(true);
  const unsigned sends = g_sends;
  for (unsigned i = 0; i < 2000U; ++i) {  // newest released: no backlog TX
    later.poll();
    assert(idle.update(true) == Pilot::Event::kNone);
  }
  assert(g_sends == sends && !idle.sent() && !idle.blocked());
  ObservationStore::Record oldest{};
  assert(later.oldestRetained(oldest) == ObservationStore::LookupResult::kFound &&
         oldest.handle.identity.sequence == 1U && !oldest.released);
  // Back home: switch to live-first and the backlog drains from the oldest.
  idle.setLiveOnly(false);
  idle.setLiveFirst(true);
  assert(sendOne(later, idle).sequence == 1U);
}
}  // namespace

int main() {
  successAndReboot(false);
  successAndReboot(true);
  cacheFailureKeepsRecord();
  liveFirst();
  liveOnly();
  puts("SF5K3V exact-cache-before-RF/same-bytes-reboot/fault + SF5L4 live-first/live-only host: PASS");
}
