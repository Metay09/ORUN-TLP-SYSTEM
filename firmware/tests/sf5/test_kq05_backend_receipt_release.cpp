// KQ05 host-only: REAL SecurityStore root + independent OpenSSL primitive
// verification of an independently computed Python HKDF/HMAC oracle. Real
// ProductRecordStore / OBS1 fake NOR release markers, restart and selective gap.
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "kq05_backend_receipt_release_owner.h"

#define main sf5k3m_previous_main
#include "test_sf5k3m_real_custody_release.cpp"
#undef main

namespace {
using Batch = orun_tlp::Kq05BackendReceiptReleaseOwner;
using Verifier = orun_tlp::Kq05BackendReceiptVerifier;

const char kReceipt[] =
 "020900040102030405060708000000000000006400000007"
 "00000001000000020000000400000005"
 "b2ee2d1e5f3619235c8d210ef1db4a28";
const char kUnknown[] =
 "020900040102030405060708000000000000006400000007"
 "00000001000000020000000400000006"
 "1739c146f4792d0ad8f7ab0d93b944b2";

void oracle(const char* hex, uint8_t (&out)[56]) {
  assert(strlen(hex) == sizeof(out)*2U);
  for (size_t i = 0U; i < sizeof(out); ++i)
    out[i] = static_cast<uint8_t>((nibble(hex[i*2U]) << 4U) |
                                   nibble(hex[i*2U+1U]));
}
void selective(bool async) {
  Flash sf;
  SecurityStore security(sf, sf);
  provisionV3(sf, security);  // fake public test credentials (never deployed).
  ProductFlash flash;
  flash.async = async;
  Source source;
  ProductRecordStore records(flash, kProductPages, &source);
  const product_region::Jobs tracker{false, true};
  records.begin(kDevice, tracker);
  assert(records.mounted());
  osf::RecordIdentity id[5]{};
  for (size_t i = 0U; i < 5U; ++i) {
    id[i] = append(records);
    assert(id[i].incarnation == 100U);
    assert(id[i].sequence == i+1U);
    assertRetained(records, id[i], false);
  }
  uint16_t cache_count = 99U;
  assert(records.store().exactObjectCount(cache_count) &&
         cache_count == 0U);  // No legacy 4-slot cache at all.

  uint8_t wire[56]{};
  oracle(kReceipt, wire);
  uint8_t unknown[56]{};
  oracle(kUnknown, unknown);
  Batch release(security, records);
  assert(release.submit(unknown, sizeof(unknown)) ==
         Batch::Submit::kStorageUnavailable);  // authentic but absent #6
  assert(release.submit(wire, sizeof(wire)-1U) ==
         Batch::Submit::kAuthenticationRejected);
  for (size_t i = 0U; i < sizeof(wire); ++i) {
    uint8_t tampered[56]{};
    memcpy(tampered, wire, sizeof(wire));
    tampered[i] ^= 1U;
    assert(release.submit(tampered, sizeof(tampered)) ==
           Batch::Submit::kAuthenticationRejected);
  }
  // Pending local Flash append must defer, not count as a forged MAC.
  // Once that unrelated write commits the exact same receipt remains valid.
  assert(records.submitPeriodic(period()) ==
         ProductRecordStore::Submit::kAccepted);
  assert(release.submit(wire, sizeof(wire)) ==
         Batch::Submit::kStorageUnavailable);
  bool append_done = false;
  for (unsigned n = 0U; n < 20000U; ++n) {
    records.poll();
    ProductRecordStore::Outcome appended{};
    if (records.takeOutcome(appended)) {
      assert(appended.stored);
      append_done = true;
      break;
    }
  }
  assert(append_done);
  assert(release.submit(wire, sizeof(wire)) == Batch::Submit::kStarted);
  assert(release.submit(wire, sizeof(wire)) == Batch::Submit::kBusy);
  Batch::Service outcome = Batch::Service::kWaitingFlash;
  for (unsigned i = 0U; i < 20000U; ++i) {
    records.poll();
    outcome = release.service();
    if (outcome == Batch::Service::kApplied ||
        outcome == Batch::Service::kFailed) break;
  }
  assert(outcome == Batch::Service::kApplied && !release.pending());
  assert(release.service() == Batch::Service::kIdle);
  assertRetained(records, id[0], true);
  assertRetained(records, id[1], true);
  assertRetained(records, id[2], false);  // #3 missing from receipt; preserved
  assertRetained(records, id[3], true);
  assertRetained(records, id[4], true);

  // Reboot: same HMAC may be replayed; all previously released identities
  // remain released and no second Flash mutation is initiated.
  Source reboot_source;
  ProductRecordStore reboot(flash, kProductPages, &reboot_source);
  reboot.begin(kDevice, tracker);
  assert(reboot.mounted());
  Batch repeated(security, reboot);
  assert(repeated.submit(wire, sizeof(wire)) == Batch::Submit::kStarted);
  assert(repeated.service() == Batch::Service::kDuplicateOnly);
  assert(repeated.service() == Batch::Service::kIdle);
  for (size_t i=0U; i<5U; ++i)
    assertRetained(reboot, id[i], i != 2U);
}
} // namespace

int main() {
  selective(false);
  selective(true);
  puts("KQ05 real-credential HMAC receipt -> durable selective OBS1 markers/reboot: PASS");
  return 0;
}
