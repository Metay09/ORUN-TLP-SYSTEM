#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <array>

#include "geofence_store.h"
#include "journal_format.h"
#include "storage_config.h"

using namespace orun_tlp;

namespace {

constexpr uint32_t kPageSize = storage_config::kPageSize;
constexpr uint32_t kRegionSize =
    kPageSize * storage_config::kGeofenceRegionPages;

class FakeFlash : public FlashBackend {
 public:
  std::array<uint8_t, kRegionSize> bytes{};
  bool fail_begin = false;
  int fail_program_at = -1;
  int partial_program_at = -1;
  size_t partial_program_bytes = 0;
  int program_then_fail_at = -1;
  int fail_erase_at = -1;
  int partial_erase_at = -1;
  size_t partial_erase_bytes = 0;
  mutable int fail_read_at = -1;
  bool unreconciled = false;
  bool mark_unreconciled_on_program_then_fail = false;
  uint32_t program_calls = 0;
  uint32_t erase_calls = 0;
  mutable uint32_t read_calls = 0;

  FakeFlash() { bytes.fill(0xFF); }

  bool begin() override { return !fail_begin; }

  bool read(uint32_t offset, void* data, size_t size) const override {
    ++read_calls;
    if (static_cast<int>(read_calls) == fail_read_at) return false;
    if (data == nullptr || offset > bytes.size() ||
        size > bytes.size() - offset)
      return false;
    memcpy(data, bytes.data() + offset, size);
    return true;
  }

  FlashOpResult program(uint32_t offset, const void* data,
                        size_t size) override {
    ++program_calls;
    if (data == nullptr || size == 0 || (offset & 3U) != 0 ||
        (size & 3U) != 0 || offset > bytes.size() ||
        size > bytes.size() - offset)
      return FlashOpResult::kFailed;
    for (size_t i = 0; i < size; ++i)
      if (bytes[offset + i] != 0xFF) return FlashOpResult::kFailed;

    const auto* source = static_cast<const uint8_t*>(data);
    if (static_cast<int>(program_calls) == fail_program_at)
      return FlashOpResult::kFailed;

    if (static_cast<int>(program_calls) == partial_program_at) {
      const size_t count =
          partial_program_bytes < size ? partial_program_bytes : size;
      for (size_t i = 0; i < count; ++i)
        bytes[offset + i] &= source[i];
      return FlashOpResult::kFailed;
    }

    for (size_t i = 0; i < size; ++i)
      bytes[offset + i] &= source[i];

    if (static_cast<int>(program_calls) == program_then_fail_at) {
      if (mark_unreconciled_on_program_then_fail) unreconciled = true;
      return FlashOpResult::kFailed;
    }
    return FlashOpResult::kDone;
  }

  FlashOpResult erasePage(uint32_t page) override {
    ++erase_calls;
    if (page >= storage_config::kGeofenceRegionPages)
      return FlashOpResult::kFailed;
    if (static_cast<int>(erase_calls) == fail_erase_at)
      return FlashOpResult::kFailed;
    if (static_cast<int>(erase_calls) == partial_erase_at) {
      const size_t count =
          partial_erase_bytes < kPageSize ? partial_erase_bytes : kPageSize;
      memset(bytes.data() + size_t(page) * kPageSize, 0xFF, count);
      return FlashOpResult::kFailed;
    }
    memset(bytes.data() + size_t(page) * kPageSize, 0xFF, kPageSize);
    return FlashOpResult::kDone;
  }

  bool hasUnreconciledMutation() const override { return unreconciled; }
};

class DeterministicIncarnation : public GeofenceIncarnationSource {
 public:
  uint64_t next = 0x1122334455667788ULL;
  unsigned calls = 0;
  bool fail = false;

  bool generate(uint64_t& incarnation) override {
    ++calls;
    if (fail) return false;
    incarnation = next;
    return true;
  }
};

geofence_format::Snapshot triangleSnapshot(int32_t delta = 0) {
  const GeoPointE7 vertices[] = {
      GeoPointE7(10000000 + delta, 20000000),
      GeoPointE7(10010000 + delta, 20000000),
      GeoPointE7(10000000 + delta, 20010000),
  };
  const GeofencePolygonView polygon(vertices, 3);
  geofence_format::Snapshot snapshot;
  assert(geofence_format::canonicalizeConfiguredAreaSet(
      GeofenceAreaSetView(&polygon, 1), snapshot));
  return snapshot;
}

bool requestTriangle(GeofenceStore& store, int32_t delta = 0) {
  const GeoPointE7 vertices[] = {
      GeoPointE7(10000000 + delta, 20000000),
      GeoPointE7(10010000 + delta, 20000000),
      GeoPointE7(10000000 + delta, 20010000),
  };
  const GeofencePolygonView polygon(vertices, 3);
  return store.requestReplace(GeofenceAreaSetView(&polygon, 1));
}

void settle(GeofenceStore& store, unsigned limit = 100) {
  for (unsigned i = 0; i < limit && store.busy(); ++i) store.poll();
  assert(!store.busy());
  // One extra pass performs post-failure reconciliation if needed.
  store.poll();
}

void writeCommitted(FakeFlash& flash, unsigned page, uint64_t generation,
                    uint64_t incarnation, uint32_t revision,
                    const geofence_format::Snapshot& snapshot) {
  geofence_format::Record record;
  record.generation = generation;
  record.token = geofence_format::StateToken(incarnation, revision);
  record.snapshot = snapshot;
  uint8_t bytes[geofence_format::kRecordSize];
  assert(geofence_format::encode(record, bytes, sizeof(bytes)));
  memcpy(flash.bytes.data() + size_t(page) * kPageSize,
         bytes, sizeof(bytes));
}

void writeStaged(FakeFlash& flash, unsigned page, uint64_t generation,
                 uint64_t incarnation, uint32_t revision,
                 const geofence_format::Snapshot& snapshot) {
  writeCommitted(flash, page, generation, incarnation, revision, snapshot);
  memset(flash.bytes.data() + size_t(page) * kPageSize +
             geofence_format::kCommitOffset,
         0xFF, 4);
}

geofence_format::StateToken requireToken(const GeofenceStore& store) {
  geofence_format::StateToken token;
  assert(store.stateToken(token));
  return token;
}

}  // namespace

int main() {
  // 1. Erased bytes are not CLEAR by themselves. With entropy, begin writes
  // and verifies a real authoritative CLEAR baseline.
  {
    FakeFlash flash;
    DeterministicIncarnation rng;
    GeofenceStore store(flash, &rng);
    assert(store.begin());
    assert(store.ready());
    assert(store.resourceState() == GeofenceResourceState::kClear);
    assert(store.tokenState() == GeofenceTokenState::kValid);
    const auto token = requireToken(store);
    assert(token.incarnation == rng.next && token.revision == 1);
    assert(rng.calls == 1);
    assert(flash.program_calls == 2);
    assert(flash.erase_calls == 0);
    assert(store.diagnostics().baseline_commits == 1);
  }

  // 2. Blank partition with no entropy stays UNAVAILABLE and untouched.
  {
    FakeFlash flash;
    GeofenceStore store(flash);
    assert(store.begin());
    assert(store.resourceState() == GeofenceResourceState::kUnavailable);
    assert(store.tokenState() == GeofenceTokenState::kUnavailable);
    assert(!store.maintenanceResetRequired());
    assert(flash.program_calls == 0 && flash.erase_calls == 0);
    assert(!store.requestClear());
  }

  // 3. REPLACE commits to inactive page, advances exactly one revision, and
  // survives reboot. CLEAR is another whole-resource mutation.
  {
    FakeFlash flash;
    DeterministicIncarnation rng;
    GeofenceStore store(flash, &rng);
    assert(store.begin());
    assert(requestTriangle(store));
    settle(store);
    bool success = false;
    assert(store.takeMutationResult(success) && success);
    assert(store.resourceState() == GeofenceResourceState::kConfigured);
    auto token = requireToken(store);
    assert(token.revision == 2);
    assert(store.diagnostics().mutations == 1);

    GeofenceStore rebooted(flash);
    assert(rebooted.begin());
    assert(rebooted.resourceState() == GeofenceResourceState::kConfigured);
    token = requireToken(rebooted);
    assert(token.incarnation == rng.next && token.revision == 2);

    assert(rebooted.requestClear());
    settle(rebooted);
    assert(rebooted.takeMutationResult(success) && success);
    assert(rebooted.resourceState() == GeofenceResourceState::kClear);
    token = requireToken(rebooted);
    assert(token.revision == 3);
  }

  // 4. Identical canonical resource is a no-op only under VALID authority.
  {
    FakeFlash flash;
    DeterministicIncarnation rng;
    GeofenceStore store(flash, &rng);
    assert(store.begin());
    assert(store.requestClear());
    assert(!store.busy());
    assert(store.diagnostics().skipped_unchanged == 1);
    assert(flash.erase_calls == 0);
  }

  // 5. One committed page plus exact staged successor: commit is erased, so
  // prior token remains VALID and staged candidate is never published.
  {
    FakeFlash flash;
    const auto clear = geofence_format::Snapshot();
    writeCommitted(flash, 0, 1, 77, 1, clear);
    writeStaged(flash, 1, 2, 77, 2, triangleSnapshot());
    GeofenceStore store(flash);
    assert(store.begin());
    assert(store.resourceState() == GeofenceResourceState::kClear);
    assert(store.tokenState() == GeofenceTokenState::kValid);
    assert(requireToken(store).revision == 1);
  }

  // 6. Partial commit / committed corruption preserve the old committed
  // semantic snapshot only as maintenance fallback; token is UNCERTAIN.
  {
    FakeFlash flash;
    geofence_format::Snapshot clear;
    geofence_format::makeClearSnapshot(clear);
    writeCommitted(flash, 0, 1, 88, 1, clear);
    writeCommitted(flash, 1, 2, 88, 2, triangleSnapshot());
    journal_format::put32(
        flash.bytes.data() + kPageSize + geofence_format::kCommitOffset,
        0x00FFFFFFUL);

    GeofenceStore store(flash);
    assert(store.begin());
    assert(store.resourceState() == GeofenceResourceState::kClear);
    assert(store.tokenState() == GeofenceTokenState::kUncertain);
    assert(store.maintenanceResetRequired());
    geofence_format::StateToken hidden;
    assert(!store.stateToken(hidden));
    assert(!store.requestClear());
  }
  {
    FakeFlash flash;
    geofence_format::Snapshot clear;
    geofence_format::makeClearSnapshot(clear);
    writeCommitted(flash, 0, 1, 89, 1, clear);
    writeCommitted(flash, 1, 2, 89, 2, triangleSnapshot());
    flash.bytes[kPageSize + geofence_format::kVerticesOffset] ^= 1;

    GeofenceStore store(flash);
    assert(store.begin());
    assert(store.resourceState() == GeofenceResourceState::kClear);
    assert(store.tokenState() == GeofenceTokenState::kUncertain);
  }

  // 7. Two committed incarnations are contradictory authority even if the
  // semantic snapshot agrees.
  {
    FakeFlash flash;
    const auto snapshot = triangleSnapshot();
    writeCommitted(flash, 0, 1, 100, 1, snapshot);
    writeCommitted(flash, 1, 2, 200, 1, snapshot);
    GeofenceStore store(flash);
    assert(store.begin());
    assert(store.resourceState() == GeofenceResourceState::kUnavailable);
    assert(store.tokenState() == GeofenceTokenState::kUncertain);
    assert(store.maintenanceResetRequired());
  }

  // 8. Unsupported/newer evidence and dirty reserved tail are non-destructive.
  {
    FakeFlash flash;
    memset(flash.bytes.data(), 0, geofence_format::kRecordSize);
    journal_format::put32(flash.bytes.data(), geofence_format::kMagic);
    flash.bytes[4] = 4;
    GeofenceStore store(flash);
    assert(store.begin());
    assert(store.resourceState() == GeofenceResourceState::kUnavailable);
    assert(store.tokenState() == GeofenceTokenState::kUncertain);
    assert(flash.program_calls == 0 && flash.erase_calls == 0);
  }
  {
    FakeFlash flash;
    geofence_format::Snapshot clear;
    geofence_format::makeClearSnapshot(clear);
    writeCommitted(flash, 0, 1, 101, 1, clear);
    flash.bytes[geofence_format::kRecordSize + 20] = 0;
    GeofenceStore store(flash);
    assert(store.begin());
    assert(store.resourceState() == GeofenceResourceState::kClear);
    assert(store.tokenState() == GeofenceTokenState::kUncertain);
    assert(store.maintenanceResetRequired());
  }

  // 9. A commit write may physically land and still report failure. Logical
  // result remains false, then reconciliation may prove the new committed
  // successor and restore VALID authority.
  {
    FakeFlash flash;
    DeterministicIncarnation rng;
    GeofenceStore store(flash, &rng);
    assert(store.begin());
    // baseline used calls 1(body),2(commit). Mutation: erase, then body call 3,
    // commit call 4.
    flash.program_then_fail_at = 4;
    assert(requestTriangle(store, 50));
    settle(store);
    bool success = true;
    assert(store.takeMutationResult(success) && !success);
    assert(store.resourceState() == GeofenceResourceState::kConfigured);
    assert(store.tokenState() == GeofenceTokenState::kValid);
    assert(requireToken(store).revision == 2);
    assert(store.diagnostics().recovery_reconciliations == 1);
  }

  // 10. Accepted-but-unreconciled mutation blocks recovery until backend
  // ownership is definitively reconciled.
  {
    FakeFlash flash;
    DeterministicIncarnation rng;
    GeofenceStore store(flash, &rng);
    assert(store.begin());
    flash.program_then_fail_at = 4;
    flash.mark_unreconciled_on_program_then_fail = true;
    assert(requestTriangle(store, 100));
    for (unsigned i = 0; i < 10 && store.busy(); ++i) store.poll();
    assert(!store.busy());
    bool success = true;
    assert(store.takeMutationResult(success) && !success);
    assert(store.tokenState() == GeofenceTokenState::kUncertain);
    store.poll();
    assert(store.tokenState() == GeofenceTokenState::kUncertain);
    flash.unreconciled = false;
    store.poll();
    assert(store.tokenState() == GeofenceTokenState::kValid);
    assert(store.resourceState() == GeofenceResourceState::kConfigured);
  }

  // 11. Partial target-page erase after a prior mutation never destroys the
  // current active semantic snapshot, but contradictory inactive evidence
  // invalidates token authority.
  {
    FakeFlash flash;
    DeterministicIncarnation rng;
    GeofenceStore store(flash, &rng);
    assert(store.begin());
    assert(requestTriangle(store, 1));
    settle(store);
    bool success = false;
    assert(store.takeMutationResult(success) && success);

    flash.partial_erase_at = static_cast<int>(flash.erase_calls) + 1;
    flash.partial_erase_bytes = 128;
    assert(requestTriangle(store, 2));
    settle(store);
    assert(store.takeMutationResult(success) && !success);
    assert(store.resourceState() == GeofenceResourceState::kConfigured);
    assert(store.tokenState() == GeofenceTokenState::kUncertain);
  }

  // 12. Read failure during boot is a backend initialization failure, never a
  // fabricated CLEAR/CONFIGURED state.
  {
    FakeFlash flash;
    flash.fail_read_at = 1;
    GeofenceStore store(flash);
    assert(!store.begin());
    assert(!store.ready());
  }

  puts("M6D3B GeofenceStore recovery/mutation checks: PASS");
}
