// M7P6F: SecurityStore v2 + v1 migration against the actual production class. A portable fake
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

#include "journal_format.h"
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
  int fail_at_program_call = -1;  // clean failure; writes no byte.
  int partial_program_at_call = -1;  // writes prefix then reports failure.
  size_t partial_program_bytes = 0;
  int program_then_fail_at_call = -1;  // full mutation, ambiguous failure.
  int fail_at_erase_call = -1;
  int partial_erase_at_call = -1;
  size_t partial_erase_bytes = 0;
  mutable int fail_at_read_call = -1;
  uint32_t program_calls = 0, erase_calls = 0;
  mutable uint32_t read_calls = 0;

  FakeFlash() { bytes.fill(0xFF); }
  bool begin() override { return !fail_begin; }

  bool read(uint32_t offset, void* data, size_t size) const override {
    ++read_calls;
    if (static_cast<int>(read_calls) == fail_at_read_call) return false;
    if (data == nullptr || offset > bytes.size() ||
        size > bytes.size() - offset)
      return false;
    memcpy(data, bytes.data() + offset, size);
    return true;
  }

  FlashOpResult program(uint32_t offset, const void* data, size_t size) override {
    ++program_calls;
    if (data == nullptr || size == 0 || (offset & 3U) != 0 ||
        (size & 3U) != 0 || offset > bytes.size() ||
        size > bytes.size() - offset)
      return FlashOpResult::kFailed;

    // Match production NrfSecurityFlash/FlashMutationGate: any non-erased
    // destination byte rejects a fresh program request. The old AND model
    // accidentally allowed retries over dirty/torn slots.
    for (size_t index = 0; index < size; ++index)
      if (bytes[offset + index] != 0xFF) return FlashOpResult::kFailed;

    const auto* source = static_cast<const uint8_t*>(data);
    if (static_cast<int>(program_calls) == fail_at_program_call)
      return FlashOpResult::kFailed;

    if (static_cast<int>(program_calls) == partial_program_at_call) {
      const size_t count =
          partial_program_bytes < size ? partial_program_bytes : size;
      for (size_t index = 0; index < count; ++index)
        bytes[offset + index] &= source[index];
      return FlashOpResult::kFailed;
    }

    for (size_t index = 0; index < size; ++index)
      bytes[offset + index] &= source[index];
    if (static_cast<int>(program_calls) == program_then_fail_at_call)
      return FlashOpResult::kFailed;
    return FlashOpResult::kDone;
  }

  FlashOpResult erasePage(uint32_t page) override {
    ++erase_calls;
    if (page >= storage_config::kFutureSecurityRegionPages)
      return FlashOpResult::kFailed;
    if (static_cast<int>(erase_calls) == fail_at_erase_call)
      return FlashOpResult::kFailed;
    if (static_cast<int>(erase_calls) == partial_erase_at_call) {
      const size_t count =
          partial_erase_bytes < kPageSize ? partial_erase_bytes : kPageSize;
      memset(bytes.data() + size_t(page) * kPageSize, 0xFF, count);
      return FlashOpResult::kFailed;
    }
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

bool submitA2d(SecurityStore& store, uint8_t credential_seed,
               uint64_t counter, uint32_t key_epoch = 1) {
  uint8_t id[kCredentialIdSize];
  fillId(id, credential_seed);
  return store.submitAuthenticatedA2dCounter(id, key_epoch, counter);
}

void writeCredentialBytes(FakeFlash& flash, unsigned page, uint8_t seed,
                          uint64_t device = kDeviceA) {
  Credential credential{};
  fillId(credential.credential_id, seed);
  credential.key_epoch = 1;
  credential.device_identity = device;
  fillKRoot(credential.k_root, seed);
  uint8_t bytes[kCredentialRecordSize];
  encodeCredential(credential, bytes);
  memcpy(flash.bytes.data() + size_t(page) * kPageSize +
             credentialRecordOffset(),
         bytes, sizeof(bytes));
}

void writeLegacyV1Page(FakeFlash& flash, unsigned page, uint64_t generation,
                       uint8_t seed, uint64_t tx_bound,
                       uint64_t device = kDeviceA) {
  PageHeader header{generation, device};
  uint8_t header_bytes[kPageHeaderSize];
  encodePageHeaderVersion(header, kVersionV1, header_bytes);
  memcpy(flash.bytes.data() + size_t(page) * kPageSize, header_bytes,
         sizeof(header_bytes));
  writeCredentialBytes(flash, page, seed, device);

  uint8_t id[kCredentialIdSize];
  fillId(id, seed);
  unsigned slot = 0;
  for (uint64_t bound = kTxReservationBlockSize;
       bound <= tx_bound && slot < kV1TxReserveSlotsPerPage;
       bound += kTxReservationBlockSize, ++slot) {
    TxReserve reserve{};
    memcpy(reserve.credential_id, id, kCredentialIdSize);
    reserve.key_epoch = 1;
    reserve.tx_reserved_bound = bound;
    uint8_t bytes[kTxReserveRecordSize];
    encodeTxReserve(reserve, bytes);
    memcpy(flash.bytes.data() + size_t(page) * kPageSize +
               txReserveRecordOffset(slot),
           bytes, sizeof(bytes));
  }
}

void writeV2PageBase(FakeFlash& flash, unsigned page,
                     uint64_t generation, uint8_t seed,
                     uint64_t device = kDeviceA) {
  PageHeader header{generation, device};
  uint8_t header_bytes[kPageHeaderSize];
  encodePageHeader(header, header_bytes);
  memcpy(flash.bytes.data() + size_t(page) * kPageSize, header_bytes,
         sizeof(header_bytes));
  writeCredentialBytes(flash, page, seed, device);
}

void writeV2State(FakeFlash& flash, unsigned page, unsigned slot,
                  uint8_t seed, SecurityStateKind kind, uint64_t value) {
  SecurityStateRecord record{};
  fillId(record.credential_id, seed);
  record.key_epoch = 1;
  record.kind = kind;
  record.value = value;
  uint8_t bytes[kSecurityStateRecordSize];
  encodeSecurityState(record, bytes);
  memcpy(flash.bytes.data() + size_t(page) * kPageSize +
             securityStateRecordOffset(slot),
         bytes, sizeof(bytes));
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

  // 6. Torn TX reservation write: a second reservation torn mid-write must not
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

  // 7. Corrupted non-erased v2 TX state on the authoritative page must
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
    const uint32_t offset = securityStateRecordOffset(1);
    flash.bytes[offset + 24] ^= 0xFF;  // corrupt its bound/CRC relationship.

    SecurityStore recovered(flash, flash);
    assert(recovered.begin(DeviceIdentity::fromLegacyUint64(kDeviceA)));
    assert(recovered.state() == SecurityState::kFault);
    assert(recovered.diagnostics().recovery_corruptions >= 1);
    uint64_t counter = 0;
    uint32_t epoch = 0;
    assert(!recovered.reserveNextTxCounter(counter, epoch));
  }

  // 7b. A committed page whose header magic is damaged is ambiguous
  // authoritative state and must fail closed rather than looking blank.
  {
    FakeFlash flash;
    SecurityStore store(flash, flash);
    assert(store.begin(DeviceIdentity::fromLegacyUint64(kDeviceA)));
    assert(commitAndSettle(store, 51));
    flash.bytes[0] ^= 0x01;  // damage magic; activation word remains committed.

    SecurityStore recovered(flash, flash);
    assert(recovered.begin(DeviceIdentity::fromLegacyUint64(kDeviceA)));
    assert(recovered.state() == SecurityState::kFault);
    uint64_t counter = 0;
    uint32_t epoch = 0;
    assert(!recovered.reserveNextTxCounter(counter, epoch));
  }

  // 7c. Append-only SECURITY_STATE records may never contain an erased gap
  // followed by a later record; such a gap could hide a higher durable bound.
  {
    FakeFlash flash;
    SecurityStore store(flash, flash);
    assert(store.begin(DeviceIdentity::fromLegacyUint64(kDeviceA)));
    assert(commitAndSettle(store, 52));

    // slot 0 exists from provisioning; leave slot 1 erased and inject slot 2.
    writeV2State(flash, 0, 2, 52,
                 SecurityStateKind::kTxReserveExclusiveBound,
                 kTxReservationBlockSize * 3);

    SecurityStore recovered(flash, flash);
    assert(recovered.begin(DeviceIdentity::fromLegacyUint64(kDeviceA)));
    assert(recovered.state() == SecurityState::kFault);
  }

  // 7d. Authoritative device-bound v1 migrates to v2 on the inactive page.
  // The credential and highest durable TX bound are preserved exactly; reboot
  // burns unused v1 headroom and the first post-migration counter begins at
  // the old durable bound.
  {
    FakeFlash flash;
    writeLegacyV1Page(flash, 0, 1, 53,
                      kTxReservationBlockSize * 2);
    SecurityStore store(flash, flash);
    assert(store.begin(DeviceIdentity::fromLegacyUint64(kDeviceA)));
    assert(store.state() == SecurityState::kProvisioned);
    assert(store.busy());
    settle(store);
    assert(store.diagnostics().migrations == 1);

    uint8_t version = 0;
    assert(headerMagicPresent(flash.bytes.data() + kPageSize, &version));
    assert(version == kVersionV2);
    assert(journal_format::erased(flash.bytes.data(), kPageSize));

    SecurityStateRecord carried{}, next{};
    assert(decodeSecurityState(
        flash.bytes.data() + kPageSize + securityStateRecordOffset(0),
        carried));
    assert(carried.kind ==
           SecurityStateKind::kTxReserveExclusiveBound);
    assert(carried.value == kTxReservationBlockSize * 2);
    // Migration itself creates no replay state. The only later append caused
    // by begin()/settle() is the fresh TX reservation needed after reboot.
    assert(decodeSecurityState(
        flash.bytes.data() + kPageSize + securityStateRecordOffset(1),
        next));
    assert(next.kind == SecurityStateKind::kTxReserveExclusiveBound);
    assert(journal_format::erased(
        flash.bytes.data() + kPageSize + securityStateRecordOffset(2),
        kSecurityStateRecordSize));

    uint64_t counter = 0;
    uint32_t epoch = 0;
    assert(store.reserveNextTxCounter(counter, epoch));
    assert(counter == kTxReservationBlockSize * 2);
    assert(epoch == 1);
  }

  // 7e. Migration activation-last cut matrix. Program writes are:
  // header body, TX-state body+commit, credential body+commit, activation.
  // Failure at any of those six writes leaves the committed v1 page
  // authoritative and recoverable.
  for (int fail_call = 1; fail_call <= 6; ++fail_call) {
    FakeFlash flash;
    writeLegacyV1Page(flash, 0, 1, 54, kTxReservationBlockSize);
    flash.fail_at_program_call = fail_call;

    SecurityStore store(flash, flash);
    assert(store.begin(DeviceIdentity::fromLegacyUint64(kDeviceA)));
    settle(store);
    assert(store.diagnostics().migration_failures == 1);

    FakeFlash snapshot;
    snapshot.bytes = flash.bytes;
    SecurityStore recovered(snapshot, snapshot);
    assert(recovered.begin(DeviceIdentity::fromLegacyUint64(kDeviceA)));
    assert(recovered.state() == SecurityState::kProvisioned);
    uint8_t id[kCredentialIdSize], expected[kCredentialIdSize];
    fillId(expected, 54);
    assert(recovered.currentCredentialId(id));
    assert(memcmp(id, expected, kCredentialIdSize) == 0);

    uint8_t version = 0;
    assert(headerMagicPresent(snapshot.bytes.data(), &version));
    assert(version == kVersionV1);
  }

  // 7e2. If automatic migration fails, this firmware does not fall back to
  // appending fresh v1 TX records. Protected TX stays closed for the boot
  // rather than extending a legacy schema after v2-aware code is running.
  {
    FakeFlash flash;
    writeLegacyV1Page(flash, 0, 1, 85, kTxReservationBlockSize);
    flash.fail_at_program_call = 1;  // migration header body

    SecurityStore store(flash, flash);
    assert(store.begin(DeviceIdentity::fromLegacyUint64(kDeviceA)));
    settle(store);
    assert(store.diagnostics().migration_failures == 1);
    const uint32_t programs_after_failure = flash.program_calls;

    for (unsigned i = 0; i < 16; ++i) store.poll();
    assert(flash.program_calls == programs_after_failure);

    uint64_t counter = 0;
    uint32_t epoch = 0;
    assert(!store.reserveNextTxCounter(counter, epoch));
    assert(journal_format::erased(
        flash.bytes.data() + txReserveRecordOffset(1),
        kTxReserveRecordSize));
  }

  // 7f. Once v2 activation lands, failure to erase the superseded v1 page is
  // non-authoritative maintenance failure. A fresh recovery must choose the
  // higher-generation v2 page and never fall back to v1.
  {
    FakeFlash flash;
    writeLegacyV1Page(flash, 0, 1, 55, kTxReservationBlockSize);
    flash.fail_at_erase_call = 2;  // inactive-page erase succeeds; old erase fails.

    SecurityStore store(flash, flash);
    assert(store.begin(DeviceIdentity::fromLegacyUint64(kDeviceA)));
    settle(store);
    assert(store.diagnostics().migrations == 1);
    assert(store.diagnostics().old_page_erase_failures == 1);

    uint8_t old_version = 0, new_version = 0;
    assert(headerMagicPresent(flash.bytes.data(), &old_version));
    assert(headerMagicPresent(flash.bytes.data() + kPageSize, &new_version));
    assert(old_version == kVersionV1);
    assert(new_version == kVersionV2);

    FakeFlash snapshot;
    snapshot.bytes = flash.bytes;
    SecurityStore recovered(snapshot, snapshot);
    assert(recovered.begin(DeviceIdentity::fromLegacyUint64(kDeviceA)));
    assert(recovered.state() == SecurityState::kProvisioned);
    // Higher-generation v2 does not trigger another migration.
    assert(recovered.diagnostics().migrations == 0);
  }

  // 7g. FOREIGN and corrupt v1 pages are never auto-migrated or rewritten.
  {
    FakeFlash foreign_flash;
    writeLegacyV1Page(foreign_flash, 0, 1, 56,
                      kTxReservationBlockSize, kDeviceA);
    SecurityStore foreign(foreign_flash, foreign_flash);
    assert(foreign.begin(DeviceIdentity::fromLegacyUint64(kDeviceB)));
    assert(foreign.state() == SecurityState::kForeign);
    assert(!foreign.busy());
    assert(foreign_flash.program_calls == 0);
    assert(foreign_flash.erase_calls == 0);

    FakeFlash fault_flash;
    writeLegacyV1Page(fault_flash, 0, 1, 57,
                      kTxReservationBlockSize);
    fault_flash.bytes[txReserveRecordOffset(0) + 20] ^= 0x01;
    SecurityStore fault(fault_flash, fault_flash);
    assert(fault.begin(DeviceIdentity::fromLegacyUint64(kDeviceA)));
    assert(fault.state() == SecurityState::kFault);
    assert(!fault.busy());
    assert(fault_flash.program_calls == 0);
    assert(fault_flash.erase_calls == 0);
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

  // 9b. Unsupported/newer page alongside an older valid current page is
  // still a global fail-closed downgrade boundary. Older firmware cannot
  // know whether the newer-format page advanced key/counter state.
  {
    FakeFlash flash;
    SecurityStore store(flash, flash);
    assert(store.begin(DeviceIdentity::fromLegacyUint64(kDeviceA)));
    assert(commitAndSettle(store, 61));  // valid current v2 page 0.

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

  // 9c. Two committed pages with the same highest generation are ambiguous
  // authority. Never choose by page index because their bounds/credentials
  // could differ.
  {
    FakeFlash flash;
    writeV2PageBase(flash, 0, 4, 86);
    writeV2PageBase(flash, 1, 4, 86);
    writeV2State(flash, 0, 0, 86,
                 SecurityStateKind::kTxReserveExclusiveBound,
                 kTxReservationBlockSize);
    writeV2State(flash, 1, 0, 86,
                 SecurityStateKind::kTxReserveExclusiveBound,
                 kTxReservationBlockSize * 2);

    SecurityStore recovered(flash, flash);
    assert(recovered.begin(DeviceIdentity::fromLegacyUint64(kDeviceA)));
    assert(recovered.state() == SecurityState::kFault);
    assert(recovered.diagnostics().recovery_corruptions >= 1);
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

  // 11. Compaction end-to-end: exhausting one page's SECURITY_STATE capacity
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
    // One page holds kSecurityStateSlotsPerPage TX_RESERVE records; each block
    // is kTxReservationBlockSize counters. Consume enough blocks to force at
    // least one compaction (headroom is reserved at slotsPerPage-1).
    const uint64_t total = (kSecurityStateSlotsPerPage + 2) * kTxReservationBlockSize;
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
    // Provisioning already wrote SECURITY_STATE TX slot 0; consume exactly enough
    // full blocks to bring the active page to kSecurityStateSlotsPerPage - 1
    // used slots -- one short of the headroom threshold that forces
    // compaction on the NEXT reservation attempt.
    const uint64_t counters_before_compaction =
        (kSecurityStateSlotsPerPage - 1) * kTxReservationBlockSize;
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

  // 12b. Critical compaction crash point: the new page's complete snapshot
  // (header body + carried-forward TX state + credential) is durable, but
  // the FINAL page-activation word fails. Recovery must still select the old
  // page, proving activation-last prevents higher-generation rollback.
  {
    FakeFlash flash;
    SecurityStore store(flash, flash);
    assert(store.begin(DeviceIdentity::fromLegacyUint64(kDeviceA)));
    assert(commitAndSettle(store, 62));
    const uint64_t counters_before_compaction =
        (kSecurityStateSlotsPerPage - 1) * kTxReservationBlockSize;
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

  // 12c. A2D replay admission: first authenticated counter requires one
  // durable reserve; newer counters inside that exclusive bound are RAM-only,
  // while duplicate/old counters reject without any flash mutation.
  {
    FakeFlash flash;
    SecurityStore store(flash, flash);
    assert(store.begin(DeviceIdentity::fromLegacyUint64(kDeviceA)));
    assert(commitAndSettle(store, 72));

    const uint32_t writes_before = flash.program_calls;
    assert(submitA2d(store, 72, 0));
    assert(store.busy());
    settle(store);
    bool accepted = false;
    assert(store.takeA2dReplayResult(accepted) && accepted);
    const uint32_t writes_after_reserve = flash.program_calls;
    assert(writes_after_reserve == writes_before + 2);  // body + commit.

    assert(submitA2d(store, 72, 1));
    assert(store.takeA2dReplayResult(accepted) && accepted);
    assert(flash.program_calls == writes_after_reserve);

    assert(submitA2d(store, 72, 1));
    assert(store.takeA2dReplayResult(accepted) && !accepted);
    assert(flash.program_calls == writes_after_reserve);

    assert(submitA2d(store, 72, 7));
    assert(store.takeA2dReplayResult(accepted) && accepted);
    assert(flash.program_calls == writes_after_reserve);

    // Crossing the durable exclusive bound reserves the next block first.
    assert(submitA2d(store, 72, 8));
    settle(store);
    assert(store.takeA2dReplayResult(accepted) && accepted);
    assert(flash.program_calls == writes_after_reserve + 2);
  }

  // 12c2. An unread replay result belongs to the current credential lifetime.
  // Re-provisioning must refuse until the caller consumes that result.
  {
    FakeFlash flash;
    SecurityStore store(flash, flash);
    assert(store.begin(DeviceIdentity::fromLegacyUint64(kDeviceA)));
    assert(commitAndSettle(store, 80));

    assert(submitA2d(store, 72, 0));
    settle(store);

    uint8_t id[kCredentialIdSize], root[kKRootSize];
    fillId(id, 81);
    fillKRoot(root, 81);
    assert(!store.commitCredential(id, 2, root));

    bool accepted = false;
    assert(store.takeA2dReplayResult(accepted) && accepted);
    assert(store.commitCredential(id, 2, root));
    settle(store);
    bool committed = false;
    assert(store.takeCommitResult(committed) && committed);
  }

  // 12d. Large authenticated counter jumps reserve the smallest block-aligned
  // exclusive bound greater than the counter, then reboot burns all values
  // below that durable bound.
  {
    FakeFlash flash;
    SecurityStore store(flash, flash);
    assert(store.begin(DeviceIdentity::fromLegacyUint64(kDeviceA)));
    assert(commitAndSettle(store, 73));

    assert(submitA2d(store, 73, 123));
    settle(store);
    bool accepted = false;
    assert(store.takeA2dReplayResult(accepted) && accepted);

    SecurityStore recovered(flash, flash);
    assert(recovered.begin(DeviceIdentity::fromLegacyUint64(kDeviceA)));
    settle(recovered);

    const uint32_t writes_before = flash.program_calls;
    assert(submitA2d(recovered, 73, 127));
    assert(recovered.takeA2dReplayResult(accepted) && !accepted);
    assert(flash.program_calls == writes_before);

    assert(submitA2d(recovered, 73, 128));
    settle(recovered);
    assert(recovered.takeA2dReplayResult(accepted) && accepted);
  }

  // 12e. A2D reserve body failure never advances runtime replay admission and
  // reports a rejected result. Recovery sees only the prior durable bound.
  {
    FakeFlash flash;
    SecurityStore store(flash, flash);
    assert(store.begin(DeviceIdentity::fromLegacyUint64(kDeviceA)));
    assert(commitAndSettle(store, 74));

    flash.fail_at_program_call = static_cast<int>(flash.program_calls) + 1;
    assert(submitA2d(store, 72, 0));
    settle(store);
    bool accepted = true;
    assert(store.takeA2dReplayResult(accepted) && !accepted);
    assert(store.diagnostics().a2d_reservation_failures == 1);

    FakeFlash snapshot;
    snapshot.bytes = flash.bytes;
    SecurityStore recovered(snapshot, snapshot);
    assert(recovered.begin(DeviceIdentity::fromLegacyUint64(kDeviceA)));
    settle(recovered);
    assert(submitA2d(recovered, 74, 0));
    settle(recovered);
    assert(recovered.takeA2dReplayResult(accepted) && accepted);
  }

  // 12e2. A successful flash program is not enough: failed readback/verify
  // rejects the replay operation and never advances the runtime HWM.
  {
    FakeFlash flash;
    SecurityStore store(flash, flash);
    assert(store.begin(DeviceIdentity::fromLegacyUint64(kDeviceA)));
    assert(commitAndSettle(store, 82));

    assert(submitA2d(store, 72, 0));
    flash.fail_at_read_call = static_cast<int>(flash.read_calls) + 1;
    settle(store);
    bool accepted = true;
    assert(store.takeA2dReplayResult(accepted) && !accepted);
    assert(store.diagnostics().a2d_reservation_failures == 1);

    // Snapshot the durable bytes. Even if the record body/commit reached
    // flash, fresh recovery independently validates what is authoritative.
    FakeFlash snapshot;
    snapshot.bytes = flash.bytes;
    SecurityStore recovered(snapshot, snapshot);
    assert(recovered.begin(DeviceIdentity::fromLegacyUint64(kDeviceA)));
    assert(recovered.state() == SecurityState::kProvisioned);
    settle(recovered);
    // The record may in fact have reached flash before verification failed.
    // Recovery must then burn it conservatively rather than re-admit it.
    assert(submitA2d(recovered, 82, 0));
    assert(recovered.takeA2dReplayResult(accepted) && !accepted);
  }

  // 12f. A2D bound overflow cannot create an exclusive aligned bound and
  // therefore rejects fail-closed without flash mutation.
  {
    FakeFlash flash;
    SecurityStore store(flash, flash);
    assert(store.begin(DeviceIdentity::fromLegacyUint64(kDeviceA)));
    assert(commitAndSettle(store, 75));
    const uint32_t writes_before = flash.program_calls;
    const uint64_t max_bound =
        (UINT64_MAX / kA2dReplayReservationBlockSize) *
        kA2dReplayReservationBlockSize;
    assert(submitA2d(store, 75, max_bound));
    bool accepted = true;
    assert(store.takeA2dReplayResult(accepted) && !accepted);
    assert(flash.program_calls == writes_before);
    assert(store.diagnostics().a2d_exhausted_events == 1);
  }

  // 12g. Shared-log compaction carries an A2D-only snapshot before page
  // activation. The later automatic TX reserve is appended after activation.
  {
    FakeFlash flash;
    writeV2PageBase(flash, 0, 1, 76);
    for (unsigned slot = 0; slot < kSecurityStateSlotsPerPage - 1; ++slot) {
      writeV2State(flash, 0, slot, 76,
                   SecurityStateKind::kA2dReplayExclusiveBound,
                   uint64_t(slot + 1) * kA2dReplayReservationBlockSize);
    }

    SecurityStore store(flash, flash);
    assert(store.begin(DeviceIdentity::fromLegacyUint64(kDeviceA)));
    settle(store);
    assert(store.diagnostics().compactions == 1);

    SecurityStateRecord first{}, second{};
    assert(decodeSecurityState(
        flash.bytes.data() + kPageSize + securityStateRecordOffset(0),
        first));
    assert(first.kind ==
           SecurityStateKind::kA2dReplayExclusiveBound);
    assert(first.value ==
           uint64_t(kSecurityStateSlotsPerPage - 1) *
               kA2dReplayReservationBlockSize);
    assert(decodeSecurityState(
        flash.bytes.data() + kPageSize + securityStateRecordOffset(1),
        second));
    assert(second.kind == SecurityStateKind::kTxReserveExclusiveBound);
  }

  // 12h. Shared-log compaction carrying both state families writes TX then A2D
  // before activation, and only afterwards appends the next TX reservation.
  {
    FakeFlash flash;
    writeV2PageBase(flash, 0, 1, 77);
    writeV2State(flash, 0, 0, 77,
                 SecurityStateKind::kA2dReplayExclusiveBound,
                 kA2dReplayReservationBlockSize * 2);
    for (unsigned slot = 1; slot < kSecurityStateSlotsPerPage - 1; ++slot) {
      writeV2State(flash, 0, slot, 77,
                   SecurityStateKind::kTxReserveExclusiveBound,
                   uint64_t(slot) * kTxReservationBlockSize);
    }

    SecurityStore store(flash, flash);
    assert(store.begin(DeviceIdentity::fromLegacyUint64(kDeviceA)));
    settle(store);
    assert(store.diagnostics().compactions == 1);

    SecurityStateRecord tx{}, replay{}, next_tx{};
    assert(decodeSecurityState(
        flash.bytes.data() + kPageSize + securityStateRecordOffset(0), tx));
    assert(tx.kind == SecurityStateKind::kTxReserveExclusiveBound);
    assert(decodeSecurityState(
        flash.bytes.data() + kPageSize + securityStateRecordOffset(1),
        replay));
    assert(replay.kind ==
           SecurityStateKind::kA2dReplayExclusiveBound);
    assert(replay.value == kA2dReplayReservationBlockSize * 2);
    assert(decodeSecurityState(
        flash.bytes.data() + kPageSize + securityStateRecordOffset(2),
        next_tx));
    assert(next_tx.kind == SecurityStateKind::kTxReserveExclusiveBound);
    assert(next_tx.value == tx.value + kTxReservationBlockSize);
  }

  // 12i. v2 recovery rejects a decreasing bound and any programmed reserved
  // tail byte rather than silently rolling state backward or interpreting a
  // future schema extension.
  {
    FakeFlash rollback;
    writeV2PageBase(rollback, 0, 1, 78);
    writeV2State(rollback, 0, 0, 78,
                 SecurityStateKind::kTxReserveExclusiveBound,
                 kTxReservationBlockSize * 2);
    writeV2State(rollback, 0, 1, 78,
                 SecurityStateKind::kTxReserveExclusiveBound,
                 kTxReservationBlockSize);
    SecurityStore bad_order(rollback, rollback);
    assert(bad_order.begin(DeviceIdentity::fromLegacyUint64(kDeviceA)));
    assert(bad_order.state() == SecurityState::kFault);

    FakeFlash tail;
    writeV2PageBase(tail, 0, 1, 79);
    tail.bytes[securityStateRecordOffset(kSecurityStateSlotsPerPage)] = 0;
    SecurityStore bad_tail(tail, tail);
    assert(bad_tail.begin(DeviceIdentity::fromLegacyUint64(kDeviceA)));
    assert(bad_tail.state() == SecurityState::kFault);
  }

  // 12j. A CRC-valid but unknown v2 state kind is still a hard recovery
  // fault. This proves semantic kind rejection, not merely CRC corruption.
  {
    FakeFlash flash;
    writeV2PageBase(flash, 0, 1, 83);
    SecurityStateRecord record{};
    fillId(record.credential_id, 83);
    record.key_epoch = 1;
    record.kind = SecurityStateKind::kTxReserveExclusiveBound;
    record.value = kTxReservationBlockSize;
    uint8_t bytes[kSecurityStateRecordSize];
    encodeSecurityState(record, bytes);
    bytes[20] = 0x7F;  // unknown kind
    journal_format::put32(bytes + 32, journal_format::crc32(bytes, 32));
    journal_format::put32(bytes + 36, kCommit);
    memcpy(flash.bytes.data() + securityStateRecordOffset(0), bytes,
           sizeof(bytes));

    SecurityStore recovered(flash, flash);
    assert(recovered.begin(DeviceIdentity::fromLegacyUint64(kDeviceA)));
    assert(recovered.state() == SecurityState::kFault);
  }

  // 12k. Reset property: after every reboot, all counters below the last
  // durable exclusive replay bound are conservatively rejected. Progress
  // resumes only at or above that bound, so reboot can burn counters but
  // never re-accept an older authenticated counter.
  {
    FakeFlash flash;
    {
      SecurityStore initial(flash, flash);
      assert(initial.begin(DeviceIdentity::fromLegacyUint64(kDeviceA)));
      assert(commitAndSettle(initial, 84));
    }

    uint64_t next = 0;
    uint64_t durable_bound = 0;
    for (unsigned life = 0; life < 24; ++life) {
      SecurityStore store(flash, flash);
      assert(store.begin(DeviceIdentity::fromLegacyUint64(kDeviceA)));
      settle(store);

      bool accepted = true;
      if (durable_bound != 0) {
        assert(submitA2d(store, 84, durable_bound - 1));
        assert(store.takeA2dReplayResult(accepted) && !accepted);
      }

      next = durable_bound + (life % 5);
      assert(submitA2d(store, 84, next));
      if (store.busy()) settle(store);
      assert(store.takeA2dReplayResult(accepted) && accepted);
      durable_bound =
          (next / kA2dReplayReservationBlockSize + 1) *
          kA2dReplayReservationBlockSize;
    }
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
    writeV2State(flash, 0, 1, 10,
                 SecurityStateKind::kTxReserveExclusiveBound, huge_bound);

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

  puts("M7P6F SecurityStore v2/migration checks: PASS");
}
