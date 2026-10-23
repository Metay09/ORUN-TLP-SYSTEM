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
}  // namespace

int main() {
  successAndReboot(false);
  successAndReboot(true);
  cacheFailureKeepsRecord();
  puts("SF5K3V exact-cache-before-RF/same-bytes-reboot/fault host: PASS");
}
