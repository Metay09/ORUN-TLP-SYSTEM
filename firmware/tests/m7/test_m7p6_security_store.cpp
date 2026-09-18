// M7P6B: SecurityStore against the actual production class. A portable fake
// FlashBackend (no Nordic headers) models the 2-page security partition,
// entirely synchronous -- FlashMutationGate's async/pending admission
// behavior for the security client is covered separately in
// test_m7p6_flash_gate.cpp, mirroring test_m7p5_config_store.cpp (pure
// store logic) vs. test_m7p5_flash_gate.cpp (async/priority) split.
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <array>
#include <set>

#include "security_store.h"
#include "storage_config.h"

using namespace orun_tlp;
using namespace orun_tlp::security_format;

namespace {
constexpr uint32_t kPageSize = storage_config::kPageSize;
constexpr uint32_t kRegionSize = kPageSize * storage_config::kFutureSecurityRegionPages;

class FakeFlash : public FlashBackend {
 public:
  std::array<uint8_t, kRegionSize> bytes{};
  bool fail_begin = false;
  int fail_at_program_call = -1;  // 1-indexed; that call returns kFailed.
  int fail_at_erase_call = -1;
  uint32_t program_calls = 0, erase_calls = 0;

  FakeFlash() { bytes.fill(0xFF); }
  bool begin() override { return !fail_begin; }

  bool read(uint32_t offset, void* data, size_t size) const override {
    if (data == nullptr || offset > bytes.size() || size > bytes.size() - offset) return false;
    memcpy(data, bytes.data() + offset, size);
    return true;
  }

  FlashOpResult program(uint32_t offset, const void* data, size_t size) override {
    ++program_calls;
    if (static_cast<int>(program_calls) == fail_at_program_call) return FlashOpResult::kFailed;
    if (data == nullptr || size == 0 || (offset & 3U) != 0 || (size & 3U) != 0 ||
        offset > bytes.size() || size > bytes.size() - offset)
      return FlashOpResult::kFailed;
    const auto* source = static_cast<const uint8_t*>(data);
    for (size_t index = 0; index < size; ++index)
      if ((bytes[offset + index] & source[index]) != source[index]) return FlashOpResult::kFailed;
    for (size_t index = 0; index < size; ++index) bytes[offset + index] &= source[index];
    return FlashOpResult::kDone;
  }

  FlashOpResult erasePage(uint32_t page) override {
    ++erase_calls;
    if (static_cast<int>(erase_calls) == fail_at_erase_call) return FlashOpResult::kFailed;
    if (page >= storage_config::kFutureSecurityRegionPages) return FlashOpResult::kFailed;
    memset(bytes.data() + size_t(page) * kPageSize, 0xFF, kPageSize);
    return FlashOpResult::kDone;
  }
};

void settle(SecurityStore& store, unsigned max_passes = 2000) {
  for (unsigned pass = 0; pass < max_passes && store.busy(); ++pass) store.poll();
  assert(!store.busy());
}

void fillId(uint8_t (&id)[kCredentialIdSize], uint8_t seed) {
  for (size_t i = 0; i < kCredentialIdSize; ++i) id[i] = static_cast<uint8_t>(seed + i);
}
void fillKRoot(uint8_t (&k)[kKRootSize], uint8_t seed) {
  for (size_t i = 0; i < kKRootSize; ++i) k[i] = static_cast<uint8_t>(seed * 5 + i);
}

constexpr uint64_t kDeviceA = 0x0E8ADE7E71531AA3ULL;
constexpr uint64_t kDeviceB = 0x09A462BD4B275BA5ULL;

bool commitAndSettle(SecurityStore& store, uint8_t seed) {
  uint8_t id[kCredentialIdSize], k_root[kKRootSize];
  fillId(id, seed);
  fillKRoot(k_root, seed);
  if (!store.commitCredential(id, 1, k_root)) return false;
  settle(store);
  bool success = false;
  return store.takeCommitResult(success) && success;
}
}  // namespace

int main() {
  // 1. Blank flash recovers as UNPROVISIONED, ready, unprovisioned never
  // disables anything else (this store has no side effects on other
  // subsystems by construction) and issues no counters.
  {
    FakeFlash flash;
    SecurityStore store(flash, flash);
    assert(store.begin(DeviceIdentity::fromLegacyUint64(kDeviceA)));
    assert(store.ready());
    assert(store.state() == SecurityState::kUnprovisioned);
    uint64_t counter = 0;
    uint32_t epoch = 0;
    assert(!store.reserveNextTxCounter(counter, epoch));
  }

  // 2. First provisioning -> PROVISIONED, credential_id round-trips, and a
  // durable first reservation is already available after settling.
  {
    FakeFlash flash;
    SecurityStore store(flash, flash);
    assert(store.begin(DeviceIdentity::fromLegacyUint64(kDeviceA)));
    assert(commitAndSettle(store, 1));
    assert(store.state() == SecurityState::kProvisioned);
    uint8_t id[kCredentialIdSize], expected[kCredentialIdSize];
    fillId(expected, 1);
    assert(store.currentCredentialId(id));
    assert(memcmp(id, expected, kCredentialIdSize) == 0);
    assert(store.currentKeyEpoch() == 1);
    uint64_t counter = 0;
    uint32_t epoch = 0;
    assert(store.reserveNextTxCounter(counter, epoch));
    assert(counter == 0);
    assert(epoch == 1);
  }

  // 3. TX counters are strictly sequential and never repeat within one block.
  {
    FakeFlash flash;
    SecurityStore store(flash, flash);
    assert(store.begin(DeviceIdentity::fromLegacyUint64(kDeviceA)));
    assert(commitAndSettle(store, 2));
    std::set<uint64_t> seen;
    for (int i = 0; i < 200; ++i) {
      uint64_t counter = 0;
      uint32_t epoch = 0;
      assert(store.reserveNextTxCounter(counter, epoch));
      assert(seen.insert(counter).second);
      assert(epoch == 1);
    }
  }

  // 4. Reboot/recovery: credential and TX position survive a fresh
  // SecurityStore over the same bytes, and the reboot-time reservation skip
  // means the next issued counter is never less than what was durably
  // committed before the reboot.
  {
    FakeFlash flash;
    SecurityStore store(flash, flash);
    assert(store.begin(DeviceIdentity::fromLegacyUint64(kDeviceA)));
    assert(commitAndSettle(store, 3));
    for (int i = 0; i < 10; ++i) {
      uint64_t counter = 0;
      uint32_t epoch = 0;
      assert(store.reserveNextTxCounter(counter, epoch));
    }
    SecurityStore recovered(flash, flash);
    assert(recovered.begin(DeviceIdentity::fromLegacyUint64(kDeviceA)));
    assert(recovered.state() == SecurityState::kProvisioned);
    uint8_t id[kCredentialIdSize], expected[kCredentialIdSize];
    fillId(expected, 3);
    assert(recovered.currentCredentialId(id));
    assert(memcmp(id, expected, kCredentialIdSize) == 0);
    // The first block (0..255) was only partially consumed (10 counters);
    // reboot must never reuse 10..255 -- the fresh reservation forced by
    // begin() starts the next durable block at exactly 256.
    settle(recovered);
    uint64_t counter = 0;
    uint32_t epoch = 0;
    assert(recovered.reserveNextTxCounter(counter, epoch));
    assert(counter == kTxReservationBlockSize);
  }

  // 5. Torn first provisioning at every durable program stage. The page
  // header commit is the FINAL activation write, so no earlier partial state
  // may be adopted as a provisioned credential.
  for (int fail_call = 1; fail_call <= 4; ++fail_call) {
    FakeFlash flash;
    flash.fail_at_program_call = fail_call;
    SecurityStore store(flash, flash);
    assert(store.begin(DeviceIdentity::fromLegacyUint64(kDeviceA)));
    uint8_t id[kCredentialIdSize], k_root[kKRootSize];
    fillId(id, 1);
    fillKRoot(k_root, 1);
    assert(store.commitCredential(id, 1, k_root));
    settle(store);
    bool success = true;
    assert(store.takeCommitResult(success) && !success);

    FakeFlash snapshot;
    snapshot.bytes = flash.bytes;
    SecurityStore recovered(snapshot, snapshot);
    assert(recovered.begin(DeviceIdentity::fromLegacyUint64(kDeviceA)));
    assert(recovered.state() == SecurityState::kUnprovisioned);
  }

  // 6. Torn TX_RESERVE write: a second reservation torn mid-write must not
  // roll the durable bound backward or lose the first block's high-water
  // mark, and no counter from the torn block is ever available.
  {
    FakeFlash flash;
    SecurityStore store(flash, flash);
    assert(store.begin(DeviceIdentity::fromLegacyUint64(kDeviceA)));
    assert(commitAndSettle(store, 4));
    for (uint64_t i = 0; i < kTxReservationBlockSize; ++i) {
      uint64_t counter = 0;
      uint32_t epoch = 0;
      assert(store.reserveNextTxCounter(counter, epoch));
    }
    // Block exhausted: the next poll() should begin reserving block 2.
    // Fail the very next program() call (the reservation's body write).
    flash.fail_at_program_call = static_cast<int>(flash.program_calls) + 1;
    store.poll();  // kick off the auto-triggered reservation (job_ starts kNone).
    settle(store);
    uint64_t counter = 0;
    uint32_t epoch = 0;
    assert(!store.reserveNextTxCounter(counter, epoch));  // torn: no durable block 2.
    assert(store.diagnostics().reservation_failures >= 1);

    FakeFlash snapshot;
    snapshot.bytes = flash.bytes;
    SecurityStore recovered(snapshot, snapshot);
    assert(recovered.begin(DeviceIdentity::fromLegacyUint64(kDeviceA)));
    assert(recovered.state() == SecurityState::kProvisioned);
    settle(recovered);
    assert(recovered.reserveNextTxCounter(counter, epoch));
    // Recovery must never issue anything below the already-consumed range,
    // and never anything from the torn attempt at 512 before a fresh durable
    // commit -- the safe next value is exactly the old durable bound (256).
    assert(counter == kTxReservationBlockSize);
  }

  // 7. Corrupted non-erased TX_RESERVE on the authoritative page must
  // fail closed. Falling back to an earlier/lower bound could reissue
  // counters that were already durably reserved and used before corruption.
  {
    FakeFlash flash;
    SecurityStore store(flash, flash);
    assert(store.begin(DeviceIdentity::fromLegacyUint64(kDeviceA)));
    assert(commitAndSettle(store, 5));
    for (uint64_t i = 0; i < kTxReservationBlockSize; ++i) {
      uint64_t counter = 0;
      uint32_t epoch = 0;
      assert(store.reserveNextTxCounter(counter, epoch));
    }
    store.poll();
    settle(store);  // second reservation fully lands.
    const uint32_t offset = txReserveRecordOffset(1);
    flash.bytes[offset + 20] ^= 0xFF;  // corrupt its bound/CRC relationship.

    SecurityStore recovered(flash, flash);
    assert(recovered.begin(DeviceIdentity::fromLegacyUint64(kDeviceA)));
    assert(recovered.state() == SecurityState::kFault);
    assert(recovered.diagnostics().recovery_corruptions >= 1);
    uint64_t counter = 0;
    uint32_t epoch = 0;
    assert(!recovered.reserveNextTxCounter(counter, epoch));
  }

  // 8. DeviceIdentity mismatch: a structurally valid credential bound to a
  // different device recovers as kForeign, never adopted, no counters, and
  // commitCredential() is refused outright (no destructive rewrite).
  {
    FakeFlash flash;
    {
      SecurityStore provisioner(flash, flash);
      assert(provisioner.begin(DeviceIdentity::fromLegacyUint64(kDeviceA)));
      assert(commitAndSettle(provisioner, 6));
    }
    SecurityStore store(flash, flash);
    assert(store.begin(DeviceIdentity::fromLegacyUint64(kDeviceB)));
    assert(store.state() == SecurityState::kForeign);
    uint64_t counter = 0;
    uint32_t epoch = 0;
    assert(!store.reserveNextTxCounter(counter, epoch));
    uint8_t id[kCredentialIdSize], k_root[kKRootSize];
    fillId(id, 9);
    fillKRoot(k_root, 9);
    assert(!store.commitCredential(id, 1, k_root));
    // Flash bytes are completely untouched by the foreign recovery attempt.
    FakeFlash original;
    {
      SecurityStore provisioner(original, original);
      assert(provisioner.begin(DeviceIdentity::fromLegacyUint64(kDeviceA)));
      assert(commitAndSettle(provisioner, 6));
    }
    assert(memcmp(flash.bytes.data(), original.bytes.data(), kRegionSize) == 0);
  }

  // 9. Unsupported/newer format: a recognized magic with an unrecognized
  // version recovers as kUnsupported, is never destructively "repaired",
  // and refuses commitCredential() rather than silently overwriting it.
  {
    FakeFlash flash;
    PageHeader header{1, kDeviceA};
    uint8_t bytes[kPageHeaderSize];
    encodePageHeader(header, bytes);
    bytes[4] = kVersion + 1;  // corrupt only the version byte after sealing.
    memcpy(flash.bytes.data(), bytes, sizeof(bytes));

    SecurityStore store(flash, flash);
    assert(store.begin(DeviceIdentity::fromLegacyUint64(kDeviceA)));
    assert(store.state() == SecurityState::kUnsupported);
    uint8_t id[kCredentialIdSize], k_root[kKRootSize];
    fillId(id, 9);
    fillKRoot(k_root, 9);
    assert(!store.commitCredential(id, 1, k_root));
    assert(flash.bytes[4] == kVersion + 1);  // untouched.
  }

  // 9b. Unsupported/newer page alongside an older valid v1 page is
  // still a global fail-closed downgrade boundary. Older firmware cannot
  // know whether the newer-format page advanced key/counter state.
  {
    FakeFlash flash;
    SecurityStore store(flash, flash);
    assert(store.begin(DeviceIdentity::fromLegacyUint64(kDeviceA)));
    assert(commitAndSettle(store, 61));  // valid v1 page 0.

    PageHeader newer{2, kDeviceA};
    uint8_t header[kPageHeaderSize];
    encodePageHeader(newer, header);
    header[4] = kVersion + 1;
    memcpy(flash.bytes.data() + kPageSize, header, sizeof(header));

    SecurityStore recovered(flash, flash);
    assert(recovered.begin(DeviceIdentity::fromLegacyUint64(kDeviceA)));
    assert(recovered.state() == SecurityState::kUnsupported);
    uint64_t counter = 0;
    uint32_t epoch = 0;
    assert(!recovered.reserveNextTxCounter(counter, epoch));
  }

  // 10. Valid older page survives a damaged newer page (interrupted
  // compaction/re-provision at various stages): the store must keep using
  // the old, fully valid page and never adopt torn newer-generation bytes.
  {
    FakeFlash flash;
    SecurityStore store(flash, flash);
    assert(store.begin(DeviceIdentity::fromLegacyUint64(kDeviceA)));
    assert(commitAndSettle(store, 7));  // page 0 fully committed, generation 1.
    // Hand-craft a torn, higher-generation page 1 (header only, no credential).
    PageHeader torn_header{2, kDeviceA};
    uint8_t header_bytes[kPageHeaderSize];
    encodePageHeader(torn_header, header_bytes);
    memset(header_bytes + kPageHeaderSize - sizeof(uint32_t), 0xFF, sizeof(uint32_t));
    memcpy(flash.bytes.data() + kPageSize, header_bytes, sizeof(header_bytes));
    // Header body exists but page activation + credential never completed.

    SecurityStore recovered(flash, flash);
    assert(recovered.begin(DeviceIdentity::fromLegacyUint64(kDeviceA)));
    assert(recovered.state() == SecurityState::kProvisioned);
    uint8_t id[kCredentialIdSize], expected[kCredentialIdSize];
    fillId(expected, 7);
    assert(recovered.currentCredentialId(id));
    assert(memcmp(id, expected, kCredentialIdSize) == 0);
  }

  // 10b. Once a newer page activation word is committed, invalid
  // credential state beneath it is ambiguous post-commit corruption and must
  // fail closed rather than falling back to an older generation.
  {
    FakeFlash flash;
    SecurityStore store(flash, flash);
    assert(store.begin(DeviceIdentity::fromLegacyUint64(kDeviceA)));
    assert(commitAndSettle(store, 71));

    PageHeader newer{2, kDeviceA};
    uint8_t header[kPageHeaderSize];
    encodePageHeader(newer, header);
    memcpy(flash.bytes.data() + kPageSize, header, sizeof(header));
    // Leave page-1 credential erased despite an activated header.

    SecurityStore recovered(flash, flash);
    assert(recovered.begin(DeviceIdentity::fromLegacyUint64(kDeviceA)));
    assert(recovered.state() == SecurityState::kFault);
    uint64_t counter = 0;
    uint32_t epoch = 0;
    assert(!recovered.reserveNextTxCounter(counter, epoch));
  }

  // 11. Compaction end-to-end: exhausting one page's TX_RESERVE capacity
  // forces a real compaction onto the other page before the active page
  // could ever be discovered completely full. The credential and durable TX
  // position survive; the old page is erased; no counter is ever repeated
  // through the transition.
  {
    FakeFlash flash;
    SecurityStore store(flash, flash);
    assert(store.begin(DeviceIdentity::fromLegacyUint64(kDeviceA)));
    assert(commitAndSettle(store, 8));
    std::set<uint64_t> seen;
    // One page holds kTxReserveSlotsPerPage TX_RESERVE records; each block
    // is kTxReservationBlockSize counters. Consume enough blocks to force at
    // least one compaction (headroom is reserved at slotsPerPage-1).
    const uint64_t total = (kTxReserveSlotsPerPage + 2) * kTxReservationBlockSize;
    for (uint64_t i = 0; i < total; ++i) {
      uint64_t counter = 0;
      uint32_t epoch = 0;
      while (!store.reserveNextTxCounter(counter, epoch)) {
        store.poll();
      }
      assert(seen.insert(counter).second);
      assert(counter == i);
    }
    assert(store.diagnostics().compactions >= 1);
    uint8_t id[kCredentialIdSize], expected[kCredentialIdSize];
    fillId(expected, 8);
    assert(store.currentCredentialId(id));
    assert(memcmp(id, expected, kCredentialIdSize) == 0);

    // Recovery after compaction still reflects exactly the right position.
    SecurityStore recovered(flash, flash);
    assert(recovered.begin(DeviceIdentity::fromLegacyUint64(kDeviceA)));
    assert(recovered.state() == SecurityState::kProvisioned);
    settle(recovered);
    uint64_t counter = 0;
    uint32_t epoch = 0;
    assert(recovered.reserveNextTxCounter(counter, epoch));
    assert(seen.insert(counter).second);  // still no duplicate versus the pre-reboot life.
  }

  // 12. Interrupted compaction: fail the erase of the fresh compaction page
  // partway through. The OLD page must remain authoritative; no counter
  // duplication or rollback occurs, and the store remains usable afterward.
  {
    FakeFlash flash;
    SecurityStore store(flash, flash);
    assert(store.begin(DeviceIdentity::fromLegacyUint64(kDeviceA)));
    assert(commitAndSettle(store, 9));
    // Provisioning already wrote TX_RESERVE slot 0; consume exactly enough
    // full blocks to bring the active page to kTxReserveSlotsPerPage - 1
    // used slots -- one short of the headroom threshold that forces
    // compaction on the NEXT reservation attempt.
    const uint64_t counters_before_compaction =
        (kTxReserveSlotsPerPage - 1) * kTxReservationBlockSize;
    for (uint64_t i = 0; i < counters_before_compaction; ++i) {
      uint64_t counter = 0;
      uint32_t epoch = 0;
      while (!store.reserveNextTxCounter(counter, epoch)) store.poll();
    }
    // The very next reservation attempt must compact. Fail the compaction's
    // page erase outright and stop at that exact crash/failure point.
    flash.fail_at_erase_call = static_cast<int>(flash.erase_calls) + 1;
    store.poll();  // starts the auto-reservation/compaction job.
    unsigned guard = 0;
    while (store.diagnostics().reservation_failures == 0 && guard < 2000) {
      store.poll();
      ++guard;
    }
    assert(guard < 2000);
    assert(store.diagnostics().reservation_failures >= 1);
    assert(store.state() == SecurityState::kProvisioned);

    // A fresh recovery over these exact bytes still finds the old, valid page.
    SecurityStore recovered(flash, flash);
    assert(recovered.begin(DeviceIdentity::fromLegacyUint64(kDeviceA)));
    assert(recovered.state() == SecurityState::kProvisioned);
  }

  // 12b. Critical compaction crash point: after the new page header and
  // carried-forward seed TX_RESERVE are durable but before the credential
  // commits, recovery must still select the old page. This specifically
  // prevents a higher-generation page from becoming authoritative with a
  // zero/lower TX bound.
  {
    FakeFlash flash;
    SecurityStore store(flash, flash);
    assert(store.begin(DeviceIdentity::fromLegacyUint64(kDeviceA)));
    assert(commitAndSettle(store, 62));
    const uint64_t counters_before_compaction =
        (kTxReserveSlotsPerPage - 1) * kTxReservationBlockSize;
    for (uint64_t i = 0; i < counters_before_compaction; ++i) {
      uint64_t counter = 0;
      uint32_t epoch = 0;
      while (!store.reserveNextTxCounter(counter, epoch)) store.poll();
    }

    // Compaction program order is:
    // header body, seed-reserve body+commit, credential body+commit,
    // PAGE ACTIVATION last. Fail exactly that final activation write (+6).
    flash.fail_at_program_call = static_cast<int>(flash.program_calls) + 6;
    store.poll();  // starts compaction.
    unsigned guard = 0;
    while (store.diagnostics().reservation_failures == 0 && guard < 2000) {
      store.poll();
      ++guard;
    }
    assert(guard < 2000);
    assert(store.diagnostics().reservation_failures >= 1);

    SecurityStore recovered(flash, flash);
    assert(recovered.begin(DeviceIdentity::fromLegacyUint64(kDeviceA)));
    assert(recovered.state() == SecurityState::kProvisioned);
    settle(recovered);
    uint64_t counter = 0;
    uint32_t epoch = 0;
    assert(recovered.reserveNextTxCounter(counter, epoch));
    assert(counter == counters_before_compaction);
  }

  // 13. Integer overflow/wrap refusal: a durable bound already at the
  // largest representable reservation-block multiple must fail closed
  // rather than wrap, and must not roll back or silently reissue counters.
  // A fresh commitCredential() (a new credential lifetime) still recovers
  // full function -- no rollover protocol is invented; escape is via a new
  // credential only.
  {
    FakeFlash flash;
    SecurityStore store(flash, flash);
    assert(store.begin(DeviceIdentity::fromLegacyUint64(kDeviceA)));
    assert(commitAndSettle(store, 10));
    const uint64_t huge_bound = (UINT64_MAX / kTxReservationBlockSize) * kTxReservationBlockSize;
    uint8_t id[kCredentialIdSize];
    fillId(id, 10);
    TxReserve reserve{};
    memcpy(reserve.credential_id, id, kCredentialIdSize);
    reserve.key_epoch = 1;
    reserve.tx_reserved_bound = huge_bound;
    uint8_t bytes[kTxReserveRecordSize];
    encodeTxReserve(reserve, bytes);
    memcpy(flash.bytes.data() + txReserveRecordOffset(1), bytes, sizeof(bytes));

    SecurityStore recovered(flash, flash);
    assert(recovered.begin(DeviceIdentity::fromLegacyUint64(kDeviceA)));
    assert(recovered.state() == SecurityState::kProvisioned);
    settle(recovered);
    assert(recovered.exhausted());
    assert(recovered.diagnostics().exhausted_events >= 1);
    uint64_t counter = 0;
    uint32_t epoch = 0;
    assert(!recovered.reserveNextTxCounter(counter, epoch));
    // Escape hatch: a brand-new credential is unaffected by the old one's
    // exhaustion (a different credential_id trivially avoids any collision).
    assert(commitAndSettle(recovered, 11));
    assert(!recovered.exhausted());
    assert(recovered.reserveNextTxCounter(counter, epoch));
    assert(counter == 0);
  }

  // 13b. Re-provisioning cannot reset the TX counter while reusing
  // the currently active credential lifetime or current root.
  {
    FakeFlash flash;
    SecurityStore store(flash, flash);
    assert(store.begin(DeviceIdentity::fromLegacyUint64(kDeviceA)));
    assert(commitAndSettle(store, 63));

    uint8_t same_id[kCredentialIdSize], same_root[kKRootSize];
    fillId(same_id, 63);
    fillKRoot(same_root, 63);
    assert(!store.commitCredential(same_id, 2, same_root));

    uint8_t new_id[kCredentialIdSize];
    fillId(new_id, 64);
    assert(!store.commitCredential(new_id, 2, same_root));

    uint8_t new_root[kKRootSize];
    fillKRoot(new_root, 64);
    assert(store.commitCredential(new_id, 2, new_root));
    settle(store);
    bool success = false;
    assert(store.takeCommitResult(success) && success);
  }

  // 14. Ownership: an unread prior commit result must not be silently
  // overwritten by a second commitCredential() call.
  {
    FakeFlash flash;
    SecurityStore store(flash, flash);
    assert(store.begin(DeviceIdentity::fromLegacyUint64(kDeviceA)));
    uint8_t id[kCredentialIdSize], k_root[kKRootSize];
    fillId(id, 12);
    fillKRoot(k_root, 12);
    assert(store.commitCredential(id, 1, k_root));
    settle(store);
    assert(!store.busy());
    fillId(id, 13);
    fillKRoot(k_root, 13);
    assert(!store.commitCredential(id, 1, k_root));  // A's result unread.
    bool success = false;
    assert(store.takeCommitResult(success) && success);
    assert(store.commitCredential(id, 1, k_root));  // now unblocked.
    settle(store);
    assert(store.takeCommitResult(success) && success);
  }

  // 15. begin() failure: an unusable flash backend reports kFault and
  // ready() == false, and never crashes.
  {
    FakeFlash flash;
    flash.fail_begin = true;
    SecurityStore store(flash, flash);
    assert(!store.begin(DeviceIdentity::fromLegacyUint64(kDeviceA)));
    assert(!store.ready());
    assert(store.state() == SecurityState::kFault);
  }

  // 16. Property test: across many simulated reboots (some clean, some
  // torn mid-reservation), the full set of TX counters ever returned for
  // this credential_id/key_epoch must never contain a duplicate.
  {
    FakeFlash flash;
    {
      SecurityStore provisioner(flash, flash);
      assert(provisioner.begin(DeviceIdentity::fromLegacyUint64(kDeviceA)));
      assert(commitAndSettle(provisioner, 20));
    }
    std::set<uint64_t> all_counters;
    for (unsigned life = 0; life < 60; ++life) {
      SecurityStore store(flash, flash);
      assert(store.begin(DeviceIdentity::fromLegacyUint64(kDeviceA)));
      assert(store.state() == SecurityState::kProvisioned);
      const unsigned to_consume = (life * 37 + 11) % 300 + 1;
      for (unsigned i = 0; i < to_consume; ++i) {
        uint64_t counter = 0;
        uint32_t epoch = 0;
        unsigned guard = 0;
        while (!store.reserveNextTxCounter(counter, epoch) && guard < 2000) { store.poll(); ++guard; }
        assert(guard < 2000);
        assert(all_counters.insert(counter).second);
      }
      if (life % 3 == 0) {
        // Simulate a crash mid-reservation: let the store start (but not
        // finish) advancing to the next block, snapshot flash, and let the
        // NEXT life's begin()/recover() deal with whatever landed.
        uint64_t counter = 0;
        uint32_t epoch = 0;
        unsigned guard = 0;
        while (store.reserveNextTxCounter(counter, epoch)) {
          assert(all_counters.insert(counter).second);
          ++guard;
          if (guard > 5000) break;
        }
        store.poll();  // one partial step into the next reservation/compaction.
      }
      // flash.bytes IS the durable state; the next loop iteration's fresh
      // SecurityStore recovers directly from whatever is there now.
    }
    assert(all_counters.size() >= 60);
  }

  puts("M7P6B SecurityStore checks: PASS");
}
