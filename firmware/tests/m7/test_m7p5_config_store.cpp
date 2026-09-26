// ConfigStore v2 runtime checks against the production class.
//
// A portable fake FlashBackend models the fixed two-page ConfigStore region.
// Legacy v1 byte compatibility remains covered by test_m7p5_config_format.cpp;
// this file now verifies the intentional clean runtime cutover to v2.
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <array>

#include "config_store.h"
#include "journal_format.h"
#include "storage_config.h"

using namespace orun_tlp;
using namespace orun_tlp::config_format;

namespace {
constexpr uint32_t kPageSize = storage_config::kPageSize;
constexpr uint32_t kRegionSize =
    kPageSize * storage_config::kFutureConfigRegionPages;

class DeterministicIncarnation : public ConfigIncarnationSource {
 public:
  uint64_t next = 0x1122334455667788ULL;
  bool fail = false;
  uint32_t calls = 0;

  bool generate(uint64_t& incarnation) override {
    ++calls;
    if (fail || next == 0) return false;
    incarnation = next;
    return true;
  }
};

class PendingFlash : public FlashBackend {
 public:
  std::array<uint8_t, kRegionSize> bytes{};
  uint32_t pending_steps = 0;
  bool fail_on_resolve = false;
  bool fail_begin = false;
  bool mark_unreconciled_on_fail = false;
  bool unreconciled = false;
  int corrupt_after_program_call = -1;
  uint32_t program_calls = 0, erase_calls = 0, poll_calls = 0;

  PendingFlash() { bytes.fill(0xFF); }

  bool begin() override { return !fail_begin; }

  bool read(uint32_t offset, void* data, size_t size) const override {
    if (data == nullptr || offset > bytes.size() ||
        size > bytes.size() - offset)
      return false;
    memcpy(data, bytes.data() + offset, size);
    return true;
  }

  FlashOpResult program(uint32_t offset, const void* data,
                        size_t size) override {
    const uint32_t call = program_calls++;
    assert(!in_flight_);
    if (data == nullptr || size == 0 || (offset & 3U) != 0 ||
        (size & 3U) != 0 || offset > bytes.size() ||
        size > bytes.size() - offset)
      return FlashOpResult::kFailed;

    const auto* source = static_cast<const uint8_t*>(data);
    for (size_t index = 0; index < size; ++index)
      if ((bytes[offset + index] & source[index]) != source[index])
        return FlashOpResult::kFailed;
    for (size_t index = 0; index < size; ++index)
      bytes[offset + index] &= source[index];

    if (corrupt_after_program_call >= 0 &&
        static_cast<int>(call) == corrupt_after_program_call &&
        size > 0) {
      bytes[offset] ^= 0x01U;
    }

    return beginAsync();
  }

  FlashOpResult erasePage(uint32_t page) override {
    ++erase_calls;
    assert(!in_flight_);
    if (page >= storage_config::kFutureConfigRegionPages)
      return FlashOpResult::kFailed;
    memset(bytes.data() + size_t(page) * kPageSize, 0xFF, kPageSize);
    return beginAsync();
  }

  FlashOpResult pollPending() override {
    ++poll_calls;
    assert(in_flight_);
    if (remaining_ > 0) {
      --remaining_;
      return FlashOpResult::kPending;
    }
    in_flight_ = false;
    if (fail_on_resolve) {
      if (mark_unreconciled_on_fail) unreconciled = true;
      return FlashOpResult::kFailed;
    }
    return FlashOpResult::kDone;
  }

  bool hasUnreconciledMutation() const override { return unreconciled; }
  void reconcile() { unreconciled = false; }

  void seedV1(unsigned page, const Config& config, uint64_t generation) {
    uint8_t record[kRecordSize];
    encode(config, generation, record);
    memcpy(bytes.data() + size_t(page) * kPageSize, record, sizeof(record));
  }

  void seedV2(unsigned page, const V2Record& record,
              uint32_t retire_word = kErasedWord) {
    uint8_t encoded[kV2RecordSize];
    encodeV2(record, encoded);
    const size_t offset = size_t(page) * kPageSize;
    memcpy(bytes.data() + offset, encoded, sizeof(encoded));
    bytes[offset + kV2RetireOffset + 0] =
        static_cast<uint8_t>(retire_word >> 24);
    bytes[offset + kV2RetireOffset + 1] =
        static_cast<uint8_t>(retire_word >> 16);
    bytes[offset + kV2RetireOffset + 2] =
        static_cast<uint8_t>(retire_word >> 8);
    bytes[offset + kV2RetireOffset + 3] =
        static_cast<uint8_t>(retire_word);
  }

 private:
  FlashOpResult beginAsync() {
    remaining_ = pending_steps;
    if (remaining_ == 0) {
      if (fail_on_resolve) {
        if (mark_unreconciled_on_fail) unreconciled = true;
        return FlashOpResult::kFailed;
      }
      return FlashOpResult::kDone;
    }
    in_flight_ = true;
    return FlashOpResult::kPending;
  }

  bool in_flight_ = false;
  uint32_t remaining_ = 0;
};

void settle(ConfigStore& store, unsigned max_passes = 300) {
  for (unsigned pass = 0; pass < max_passes && store.busy(); ++pass)
    store.poll();
  assert(!store.busy());
}

bool saveAndSettle(ConfigStore& store, const Config& candidate,
                   bool& success) {
  if (!store.requestSave(candidate)) return false;
  if (!store.busy()) {
    success = true;  // unchanged semantic no-op
    return true;
  }
  settle(store);
  return store.takeSaveResult(success);
}

StateToken validToken(const ConfigStore& store) {
  StateToken token;
  assert(store.tokenState() == ConfigTokenState::kValid);
  assert(store.stateToken(token));
  assert(token.incarnation != 0);
  assert(token.revision != 0);
  return token;
}

PageInspection pageInspection(const PendingFlash& flash, unsigned page) {
  PageInspection inspection;
  const uint8_t* prefix =
      flash.bytes.data() + size_t(page) * kPageSize;
  assert(inspectPagePrefix(prefix, kV2PagePrefixSize, inspection));
  return inspection;
}

}  // namespace

int main() {
  // 1. Blank development partition + healthy CSPRNG -> fresh v2 baseline.
  {
    PendingFlash flash;
    DeterministicIncarnation rng;
    ConfigStore store(flash, &rng);
    assert(store.begin());
    assert(store.ready());
    // Internal token baseline is durable, but existing application provenance
    // still reports "default" until a semantic config override is committed.
    assert(!store.hasCommittedRecord());
    assert(!store.maintenanceResetRequired());
    assert(store.config().tracking_interval_seconds == 180);
    assert(store.config().battery_capacity_mah == 0);
    const StateToken token = validToken(store);
    assert(token.incarnation == rng.next && token.revision == 1);
    assert(rng.calls == 1);
    assert(store.diagnostics().baseline_commits == 1);
    assert(flash.erase_calls == 0);
    assert(flash.program_calls == 2);  // 44-byte stage + 4-byte commit

    const PageInspection p0 = pageInspection(flash, 0);
    assert(p0.evidence == PageEvidence::kV2Committed);
    assert(p0.generation == 1);
    assert(p0.token.incarnation == rng.next && p0.token.revision == 1);
    assert(pageInspection(flash, 1).evidence == PageEvidence::kErased);
  }

  // 2. No CSPRNG (or explicit failure) keeps safe defaults but no token,
  // no baseline and no equality/no-op authority.
  {
    PendingFlash flash;
    ConfigStore no_rng(flash);
    assert(no_rng.begin());
    assert(no_rng.ready());
    assert(!no_rng.hasCommittedRecord());
    assert(no_rng.tokenState() == ConfigTokenState::kUnavailable);
    assert(!no_rng.requestSave(Config{180, 0}));
    assert(flash.program_calls == 0 && flash.erase_calls == 0);
  }
  {
    PendingFlash flash;
    DeterministicIncarnation rng;
    rng.fail = true;
    ConfigStore store(flash, &rng);
    assert(store.begin());
    assert(store.tokenState() == ConfigTokenState::kUnavailable);
    assert(!store.hasCommittedRecord());
    assert(rng.calls == 1);
    assert(flash.program_calls == 0);
  }

  // 3. Normal semantic save increments generation and revision exactly once;
  // reboot recovers the same config/token.
  {
    PendingFlash flash;
    DeterministicIncarnation rng;
    ConfigStore store(flash, &rng);
    assert(store.begin());
    const StateToken before = validToken(store);

    bool success = false;
    assert(saveAndSettle(store, Config{247, 9000}, success) && success);
    const StateToken after = validToken(store);
    assert(after.incarnation == before.incarnation);
    assert(after.revision == before.revision + 1);
    assert(store.config().tracking_interval_seconds == 247);
    assert(store.config().battery_capacity_mah == 9000);
    assert(store.hasCommittedRecord());

    ConfigStore recovered(flash);
    assert(recovered.begin());
    assert(recovered.config().tracking_interval_seconds == 247);
    assert(recovered.config().battery_capacity_mah == 9000);
    assert(recovered.hasCommittedRecord());
    const StateToken reboot = validToken(recovered);
    assert(reboot.incarnation == after.incarnation);
    assert(reboot.revision == after.revision);
  }

  // 4. Two successful saves produce exact adjacent A/B lineage; highest
  // committed generation/revision wins after reboot.
  {
    PendingFlash flash;
    DeterministicIncarnation rng;
    ConfigStore store(flash, &rng);
    assert(store.begin());
    bool success = false;
    assert(saveAndSettle(store, Config{200, 1000}, success) && success);
    assert(saveAndSettle(store, Config{400, 2000}, success) && success);
    const StateToken current = validToken(store);
    assert(current.revision == 3);

    ConfigStore recovered(flash);
    assert(recovered.begin());
    assert(recovered.config().tracking_interval_seconds == 400);
    assert(validToken(recovered).revision == 3);
  }

  // 5. Exact unchanged config is zero-wear and token-stable.
  {
    PendingFlash flash;
    DeterministicIncarnation rng;
    ConfigStore store(flash, &rng);
    assert(store.begin());
    bool success = false;
    assert(saveAndSettle(store, Config{300, 50}, success) && success);
    const StateToken before = validToken(store);
    const uint32_t erases = flash.erase_calls;
    const uint32_t programs = flash.program_calls;

    assert(store.requestSave(Config{300, 50}));
    assert(!store.busy());
    const StateToken after = validToken(store);
    assert(after.incarnation == before.incarnation);
    assert(after.revision == before.revision);
    assert(flash.erase_calls == erases);
    assert(flash.program_calls == programs);
    assert(store.diagnostics().skipped_unchanged == 1);
  }

  // 6. Semantic validation remains ConfigStore-owned and fail-closed.
  {
    PendingFlash flash;
    DeterministicIncarnation rng;
    ConfigStore store(flash, &rng);
    assert(store.begin());
    const StateToken before = validToken(store);
    assert(!store.requestSave(Config{0, 1}));
    assert(!store.requestSave(
        Config{12u * 24u * 60u * 60u + 1u, 1}));
    const StateToken after = validToken(store);
    assert(after.revision == before.revision);
    assert(store.diagnostics().rejected_candidates == 2);
  }

  // 7. Normal reset is a semantic save under the existing incarnation.
  {
    PendingFlash flash;
    DeterministicIncarnation rng;
    ConfigStore store(flash, &rng);
    assert(store.begin());
    bool success = false;
    assert(saveAndSettle(store, Config{321, 7000}, success) && success);
    const StateToken before = validToken(store);
    assert(store.requestReset());
    settle(store);
    assert(store.takeSaveResult(success) && success);
    const StateToken after = validToken(store);
    assert(after.incarnation == before.incarnation);
    assert(after.revision == before.revision + 1);
    assert(store.config().tracking_interval_seconds == 180);
    assert(store.config().battery_capacity_mah == 0);

    const uint32_t programs = flash.program_calls;
    assert(store.requestReset());  // already defaults -> semantic no-op
    assert(!store.busy());
    assert(flash.program_calls == programs);
  }

  // 8. Any committed legacy-v1 development state is diagnostic only:
  // defaults + maintenance/reset, no automatic migration/erase/adoption.
  {
    PendingFlash flash;
    flash.seedV1(0, Config{777, 9999}, 5);
    DeterministicIncarnation rng;
    ConfigStore store(flash, &rng);
    assert(store.begin());
    assert(store.ready());
    assert(store.maintenanceResetRequired());
    assert(!store.hasCommittedRecord());
    assert(store.config().tracking_interval_seconds == 180);
    assert(store.tokenState() == ConfigTokenState::kUnavailable);
    assert(store.diagnostics().legacy_pages_seen == 1);
    assert(rng.calls == 0);
    assert(flash.program_calls == 0 && flash.erase_calls == 0);
    assert(!store.requestSave(Config{200, 1}));
    // requestReset() is a normal semantic reset, not the explicit destructive
    // maintenance erase/re-baseline operation.
    assert(!store.requestReset());
  }

  // 9. Genuine unsupported-newer evidence is never overwritten.
  {
    PendingFlash flash;
    memset(flash.bytes.data(), 0, kV2PagePrefixSize);
    flash.bytes[0] = 0x4F; flash.bytes[1] = 0x52;
    flash.bytes[2] = 0x43; flash.bytes[3] = 0x31;
    flash.bytes[4] = 4;  // deployable future namespace
    // Final classifier word must be non-FF; memset(0) satisfies it.
    DeterministicIncarnation rng;
    ConfigStore store(flash, &rng);
    assert(store.begin());
    assert(store.maintenanceResetRequired());
    assert(store.tokenState() == ConfigTokenState::kUncertain);
    assert(rng.calls == 0);
    assert(flash.program_calls == 0 && flash.erase_calls == 0);
  }

  // 10. Prefix-erased is not enough to call the whole 4096-byte page blank.
  // Dirty bytes after the owned 52-byte v2 prefix require maintenance and
  // must never be overwritten by automatic fresh-baseline creation.
  {
    PendingFlash flash;
    flash.bytes[100] = 0x00;  // outside current v2-owned prefix
    DeterministicIncarnation rng;
    ConfigStore store(flash, &rng);
    assert(store.begin());
    assert(store.maintenanceResetRequired());
    assert(store.tokenState() == ConfigTokenState::kUncertain);
    assert(!store.hasCommittedRecord());
    assert(rng.calls == 0);
    assert(flash.program_calls == 0 && flash.erase_calls == 0);
  }

  // 11. A valid committed v2 prefix with dirty unused page tail remains the
  // best semantic fallback, but token authority is invalidated.
  {
    PendingFlash flash;
    const uint64_t inc = 0x9999999999999999ULL;
    flash.seedV2(0, V2Record(2, Config{650, 42}, StateToken{inc, 2}));
    flash.bytes[256] = 0x00;
    ConfigStore store(flash);
    assert(store.begin());
    assert(store.maintenanceResetRequired());
    assert(store.tokenState() == ConfigTokenState::kUncertain);
    assert(store.config().tracking_interval_seconds == 650);
    assert(store.config().battery_capacity_mah == 42);
    StateToken hidden;
    assert(!store.stateToken(hidden));
    assert(!store.requestSave(Config{651, 43}));
  }

  // 12. One verified stage + erased page preserves semantic config only.
  // The staged token is never promoted after reboot and GET_CONFIG provenance
  // remains "default/not committed".
  {
    PendingFlash flash;
    const uint64_t inc = 0xABABABABABABABABULL;
    uint8_t stage[kV2RecordSize];
    encodeV2(V2Record(5, Config{610, 61}, StateToken{inc, 7}), stage);
    memset(stage + kV2CommitOffset, 0xFF, 4);
    memcpy(flash.bytes.data(), stage, sizeof(stage));

    ConfigStore store(flash);
    assert(store.begin());
    assert(store.maintenanceResetRequired());
    assert(store.tokenState() == ConfigTokenState::kUncertain);
    assert(store.config().tracking_interval_seconds == 610);
    assert(store.config().battery_capacity_mah == 61);
    assert(!store.hasCommittedRecord());
    StateToken hidden;
    assert(!store.stateToken(hidden));
    assert(!store.requestReset());
  }

  // 13. One verified partial-commit + erased page likewise preserves
  // semantics but never claims token or committed-override authority.
  {
    PendingFlash flash;
    const uint64_t inc = 0xCDCDCDCDCDCDCDCDULL;
    flash.seedV2(0, V2Record(6, Config{620, 62}, StateToken{inc, 8}));
    flash.bytes[kV2CommitOffset + 0] = 0x00;
    flash.bytes[kV2CommitOffset + 1] = 0xFF;
    flash.bytes[kV2CommitOffset + 2] = 0xFF;
    flash.bytes[kV2CommitOffset + 3] = 0xFF;

    ConfigStore store(flash);
    assert(store.begin());
    assert(store.maintenanceResetRequired());
    assert(store.tokenState() == ConfigTokenState::kUncertain);
    assert(store.config().tracking_interval_seconds == 620);
    assert(!store.hasCommittedRecord());
  }

  // 14. A retired committed record can preserve both semantic config and the
  // historical application fact that a committed override existed, but its
  // old token can never become VALID again.
  {
    PendingFlash flash;
    const uint64_t inc = 0xEFEFEFEFEFEFEFEFULL;
    flash.seedV2(0, V2Record(7, Config{630, 63}, StateToken{inc, 9}),
                 0xFFFFFFFEUL);

    ConfigStore store(flash);
    assert(store.begin());
    assert(store.maintenanceResetRequired());
    assert(store.tokenState() == ConfigTokenState::kUncertain);
    assert(store.config().tracking_interval_seconds == 630);
    assert(store.hasCommittedRecord());
    StateToken hidden;
    assert(!store.stateToken(hidden));
  }

  // 15. Exact two-commit lineage still identifies the newest semantic config
  // if unused tail corruption invalidates token authority.
  {
    PendingFlash flash;
    const uint64_t inc = 0x9191919191919191ULL;
    flash.seedV2(0, V2Record(20, Config{700, 70}, StateToken{inc, 20}));
    flash.seedV2(1, V2Record(21, Config{710, 71}, StateToken{inc, 21}));
    flash.bytes[kPageSize + 300] = 0x00;

    ConfigStore store(flash);
    assert(store.begin());
    assert(store.maintenanceResetRequired());
    assert(store.tokenState() == ConfigTokenState::kUncertain);
    assert(store.config().tracking_interval_seconds == 710);
    assert(store.config().battery_capacity_mah == 71);
    assert(store.hasCommittedRecord());
    StateToken hidden;
    assert(!store.stateToken(hidden));
  }

  // 16. Committed v2 + exact staged successor: committed token remains VALID;
  // stage is never promoted after reboot.
  {
    PendingFlash flash;
    const uint64_t inc = 0xAA55AA55AA55AA55ULL;
    flash.seedV2(0, V2Record(7, Config{300, 10}, StateToken{inc, 4}));
    uint8_t stage[kV2RecordSize];
    encodeV2(V2Record(8, Config{400, 11}, StateToken{inc, 5}), stage);
    memset(stage + kV2CommitOffset, 0xFF, 4);
    memcpy(flash.bytes.data() + kPageSize, stage, sizeof(stage));

    ConfigStore store(flash);
    assert(store.begin());
    assert(!store.maintenanceResetRequired());
    assert(store.config().tracking_interval_seconds == 300);
    const StateToken token = validToken(store);
    assert(token.incarnation == inc && token.revision == 4);
  }

  // 17. A staged candidate with impossible lineage invalidates token authority.
  {
    PendingFlash flash;
    const uint64_t inc = 0xAA55AA55AA55AA55ULL;
    flash.seedV2(0, V2Record(7, Config{300, 10}, StateToken{inc, 4}));
    uint8_t stage[kV2RecordSize];
    encodeV2(V2Record(8, Config{400, 11},
                      StateToken{0x123456789ULL, 1}), stage);
    memset(stage + kV2CommitOffset, 0xFF, 4);
    memcpy(flash.bytes.data() + kPageSize, stage, sizeof(stage));

    ConfigStore store(flash);
    assert(store.begin());
    assert(store.maintenanceResetRequired());
    assert(store.tokenState() == ConfigTokenState::kUncertain);
    // The impossible staged successor invalidates token authority only. The
    // independently committed semantic override remains the user-visible
    // stored config and must not be discarded.
    assert(store.config().tracking_interval_seconds == 300);
    assert(store.config().battery_capacity_mah == 10);
    assert(store.hasCommittedRecord());
    StateToken hidden;
    assert(!store.stateToken(hidden));
    assert(!store.requestSave(Config{301, 11}));
  }

  // 18. Two committed-valid pages require exact generation/revision lineage.
  {
    PendingFlash flash;
    const uint64_t inc = 0x1010101010101010ULL;
    flash.seedV2(0, V2Record(10, Config{100, 1}, StateToken{inc, 8}));
    flash.seedV2(1, V2Record(11, Config{200, 2}, StateToken{inc, 9}));
    ConfigStore store(flash);
    assert(store.begin());
    assert(store.config().tracking_interval_seconds == 200);
    assert(validToken(store).revision == 9);
  }
  {
    PendingFlash flash;
    const uint64_t inc = 0x1010101010101010ULL;
    flash.seedV2(0, V2Record(10, Config{100, 1}, StateToken{inc, 8}));
    flash.seedV2(1, V2Record(12, Config{200, 2}, StateToken{inc, 9}));
    ConfigStore store(flash);
    assert(store.begin());
    assert(store.maintenanceResetRequired());
    assert(store.tokenState() == ConfigTokenState::kUncertain);
  }

  // 19. Exact uncommitted/torn body evidence beside a committed v2 page is
  // non-authoritative; the old committed token remains VALID.
  {
    PendingFlash flash;
    const uint64_t inc = 0x2222222222222222ULL;
    flash.seedV2(0, V2Record(2, Config{500, 1}, StateToken{inc, 2}));
    uint8_t torn[kV2RecordSize];
    encodeV2(V2Record(3, Config{600, 2}, StateToken{inc, 3}), torn);
    memset(torn + 20, 0xFF, kV2RecordSize - 20);
    memcpy(flash.bytes.data() + kPageSize, torn, sizeof(torn));

    ConfigStore store(flash);
    assert(store.begin());
    assert(store.config().tracking_interval_seconds == 500);
    assert(validToken(store).revision == 2);
  }

  // 20. Supported corruption, partial commit, retired commit or semantically
  // invalid committed evidence is fail-closed maintenance in this slice.
  {
    PendingFlash flash;
    const uint64_t inc = 0x3333333333333333ULL;
    flash.seedV2(0, V2Record(2, Config{500, 1}, StateToken{inc, 2}));
    flash.bytes[kPageSize] = 0x12;  // generic supported corruption
    ConfigStore store(flash);
    assert(store.begin());
    assert(store.maintenanceResetRequired());
    assert(store.tokenState() == ConfigTokenState::kUncertain);
    // Contradictory inactive-page evidence invalidates token authority, but
    // must not hide the one intact committed semantic config from the user.
    assert(store.config().tracking_interval_seconds == 500);
    assert(store.config().battery_capacity_mah == 1);
    StateToken hidden;
    assert(!store.stateToken(hidden));
    assert(!store.requestSave(Config{501, 2}));
  }
  {
    PendingFlash flash;
    const uint64_t inc = 0x4444444444444444ULL;
    flash.seedV2(0, V2Record(2, Config{500, 1}, StateToken{inc, 2}));
    flash.seedV2(1, V2Record(3, Config{600, 2}, StateToken{inc, 3}));
    flash.bytes[kPageSize + kV2CommitOffset] = 0x00;
    flash.bytes[kPageSize + kV2CommitOffset + 1] = 0xFF;
    flash.bytes[kPageSize + kV2CommitOffset + 2] = 0xFF;
    flash.bytes[kPageSize + kV2CommitOffset + 3] = 0xFF;
    ConfigStore store(flash);
    assert(store.begin());
    assert(store.maintenanceResetRequired());
    assert(store.tokenState() == ConfigTokenState::kUncertain);
    assert(store.config().tracking_interval_seconds == 500);
    StateToken hidden;
    assert(!store.stateToken(hidden));
  }
  {
    PendingFlash flash;
    flash.seedV2(0, V2Record(1, Config{500, 1},
                             StateToken{0x5555555555555555ULL, 1}),
                 0xFFFFFFFEUL);
    ConfigStore store(flash);
    assert(store.begin());
    assert(store.maintenanceResetRequired());
  }
  {
    PendingFlash flash;
    const uint32_t out_of_range =
        12u * 24u * 60u * 60u + 1u;
    flash.seedV2(0, V2Record(1, Config{out_of_range, 1},
                             StateToken{0x6666666666666666ULL, 1}));
    ConfigStore store(flash);
    assert(store.begin());
    assert(store.maintenanceResetRequired());
    assert(store.diagnostics().recovery_corruptions >= 1);
  }

  // 21. Async normal save: erase/body/commit may each pend; result cannot
  // appear early and the final token advances exactly once.
  {
    PendingFlash flash;
    DeterministicIncarnation rng;
    ConfigStore store(flash, &rng);
    assert(store.begin());
    flash.pending_steps = 3;
    const StateToken before = validToken(store);
    assert(store.requestSave(Config{555, 42}));
    bool success = false;
    assert(!store.takeSaveResult(success));
    unsigned ticks = 0;
    while (store.busy()) {
      store.poll();
      ++ticks;
    }
    assert(ticks > 3);
    assert(store.takeSaveResult(success) && success);
    assert(validToken(store).revision == before.revision + 1);
    assert(flash.poll_calls > 0);
  }

  // 22. Body readback is verified BEFORE commit. Corrupting staged bytes
  // causes failure and never programs the commit word.
  {
    PendingFlash flash;
    DeterministicIncarnation rng;
    ConfigStore store(flash, &rng);
    assert(store.begin());
    // baseline used program calls 0/1. Normal save body is call 2.
    flash.corrupt_after_program_call = 2;
    const uint32_t programs_before = flash.program_calls;
    assert(store.requestSave(Config{777, 7}));
    settle(store);
    bool success = true;
    assert(store.takeSaveResult(success) && !success);
    // Only staged body was attempted after the erase; commit was not.
    assert(flash.program_calls == programs_before + 1);
    assert(store.config().tracking_interval_seconds == 180);
  }

  // 23. Ordinary failed mutation invalidates token until read-only recovery.
  // If recovery sees old committed + safely erased inactive page, VALID can
  // be restored without another flash write.
  {
    PendingFlash flash;
    DeterministicIncarnation rng;
    ConfigStore store(flash, &rng);
    assert(store.begin());
    flash.pending_steps = 1;
    flash.fail_on_resolve = true;
    assert(store.requestSave(Config{900, 1}));
    settle(store);
    bool success = true;
    assert(store.takeSaveResult(success) && !success);
    assert(store.tokenState() == ConfigTokenState::kUncertain);
    assert(!store.requestSave(Config{901, 2}));
    flash.fail_on_resolve = false;
    const uint32_t programs = flash.program_calls;
    store.poll();  // read-only full recovery
    assert(store.tokenState() == ConfigTokenState::kValid);
    assert(flash.program_calls == programs);
    assert(store.config().tracking_interval_seconds == 180);
  }

  // 24. Accepted-but-unreconciled timeout blocks every new mutation until the
  // backend clears physical ambiguity; late reconciliation causes full
  // two-page recovery, not a second logical success.
  {
    PendingFlash flash;
    DeterministicIncarnation rng;
    ConfigStore store(flash, &rng);
    assert(store.begin());
    flash.pending_steps = 1;
    flash.fail_on_resolve = true;
    flash.mark_unreconciled_on_fail = true;
    assert(store.requestSave(Config{901, 2}));
    settle(store);
    bool success = true;
    assert(store.takeSaveResult(success) && !success);
    assert(flash.hasUnreconciledMutation());
    assert(store.tokenState() == ConfigTokenState::kUncertain);
    assert(!store.requestSave(Config{902, 3}));

    flash.fail_on_resolve = false;
    store.poll();  // still quarantined: no recovery yet
    assert(store.tokenState() == ConfigTokenState::kUncertain);

    flash.reconcile();
    const uint32_t programs = flash.program_calls;
    store.poll();  // mandatory full recovery
    assert(flash.program_calls == programs);
    assert(store.tokenState() == ConfigTokenState::kValid);
    assert(store.config().tracking_interval_seconds == 180);
    assert(store.diagnostics().unreconciled_mutation_faults == 1);
    assert(store.diagnostics().recovery_reconciliations == 1);
    bool extra = false;
    assert(!store.takeSaveResult(extra));  // no late second app result
  }

  // 25. An unread async result blocks a different save but not an exact
  // synchronous no-op against the already committed semantic state.
  {
    PendingFlash flash;
    DeterministicIncarnation rng;
    ConfigStore store(flash, &rng);
    assert(store.begin());
    assert(store.requestSave(Config{222, 2}));
    settle(store);
    assert(store.config().tracking_interval_seconds == 222);
    assert(store.requestSave(Config{222, 2}));  // no-op first
    assert(!store.requestSave(Config{333, 3}));
    assert(store.diagnostics().blocked_pending_result == 1);
    bool success = false;
    assert(store.takeSaveResult(success) && success);
    assert(saveAndSettle(store, Config{333, 3}, success) && success);
  }

  // 26. Generation/revision exhaustion never wraps.
  {
    PendingFlash flash;
    const uint64_t inc = 0x7777777777777777ULL;
    flash.seedV2(0, V2Record(UINT64_MAX, Config{100, 1},
                             StateToken{inc, 10}));
    ConfigStore store(flash);
    assert(store.begin());
    assert(!store.requestSave(Config{101, 2}));
  }
  {
    PendingFlash flash;
    const uint64_t inc = 0x8888888888888888ULL;
    flash.seedV2(0, V2Record(10, Config{100, 1},
                             StateToken{inc, UINT32_MAX}));
    ConfigStore store(flash);
    assert(store.begin());
    assert(!store.requestSave(Config{101, 2}));
  }

  // 27. Backend begin failure still leaves safe in-RAM defaults but reports
  // store unavailable and performs no writes.
  {
    PendingFlash flash;
    flash.fail_begin = true;
    DeterministicIncarnation rng;
    ConfigStore store(flash, &rng);
    assert(!store.begin());
    assert(!store.ready());
    assert(store.config().tracking_interval_seconds == 180);
    assert(store.tokenState() == ConfigTokenState::kUnavailable);
    assert(flash.program_calls == 0);
  }

  puts("M7P5 ConfigStore v2 runtime checks: PASS");
}
