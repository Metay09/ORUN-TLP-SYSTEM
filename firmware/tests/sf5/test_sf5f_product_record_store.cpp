// SF5F step 1: ProductRecordStore, the tracker's durable PERIODIC owner on
// the product region (product_record_store.h). Portable: a fake 32-page
// flash, synchronous or asynchronous like the gate.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <vector>

#include "product_record_store.h"
#include "storage_config.h"
#include "tlp_v2_product_secure.h"

using namespace orun_tlp;
namespace pr = orun_tlp::product_region;

namespace {

constexpr uint16_t kPages = storage_config::kProductRegionPages;
constexpr uint32_t kPage = storage_config::kPageSize;

struct Op {
  bool erase;
  uint32_t offset;
  std::vector<uint8_t> data;
};

class FakeFlash : public FlashBackend {
 public:
  FakeFlash() : bytes(size_t(kPages) * kPage, 0xFF) {}

  bool begin() override { return true; }
  bool hasUnreconciledMutation() const override { return unreconciled; }
  bool read(uint32_t offset, void* data, size_t size) const override {
    ++read_calls;
    if (read_calls == fail_read_at ||
        (fail_reads_after_mutation != 0U &&
         mutation_calls >= fail_reads_after_mutation))
      return false;
    if (data == nullptr || uint64_t(offset) + size > bytes.size()) return false;
    memcpy(data, bytes.data() + offset, size);
    return true;
  }
  FlashOpResult program(uint32_t offset, const void* data,
                        size_t size) override {
    ++program_calls;
    ++mutation_calls;
    if (pending_ || data == nullptr || size == 0 || (offset & 3U) ||
        (size & 3U) || uint64_t(offset) + size > bytes.size())
      return FlashOpResult::kFailed;
    for (size_t i = 0; i < size; ++i)
      if (bytes[offset + i] != 0xFF) return FlashOpResult::kFailed;
    const auto* src = static_cast<const uint8_t*>(data);
    if (record) ops.push_back(Op{false, offset, {src, src + size}});
    if (mutation_calls == fail_mutation_at) {
      assert(torn_bytes < size);
      for (size_t i = 0; i < torn_bytes; ++i) bytes[offset + i] &= src[i];
      return FlashOpResult::kFailed;
    }
    for (size_t i = 0; i < size; ++i) bytes[offset + i] &= src[i];
    return finish();
  }
  FlashOpResult erasePage(uint32_t page) override {
    ++erase_calls;
    ++mutation_calls;
    if (pending_ || page >= kPages) return FlashOpResult::kFailed;
    if (record) ops.push_back(Op{true, page * kPage, {}});
    if (mutation_calls == fail_mutation_at) return FlashOpResult::kFailed;
    memset(bytes.data() + size_t(page) * kPage, 0xFF, kPage);
    return finish();
  }
  FlashOpResult pollPending() override {
    if (!pending_) return FlashOpResult::kFailed;
    if (polls_left_ > 0) {
      --polls_left_;
      return FlashOpResult::kPending;
    }
    pending_ = false;
    if (fail_async_completion) {
      // Bytes may already be visible, but a timeout is not completion proof.
      // Both a still-quarantined and an already-late-completed backend exist.
      unreconciled = quarantine_on_failure;
      return FlashOpResult::kFailed;
    }
    return FlashOpResult::kDone;
  }

  std::vector<uint8_t> bytes;
  std::vector<Op> ops;
  bool record = false;
  bool async = false;
  unsigned program_calls = 0;
  unsigned erase_calls = 0;
  unsigned mutation_calls = 0;
  mutable unsigned read_calls = 0;
  unsigned fail_mutation_at = 0;
  size_t torn_bytes = 0;
  unsigned fail_read_at = 0;
  unsigned fail_reads_after_mutation = 0;
  bool fail_async_completion = false;
  bool quarantine_on_failure = false;
  bool unreconciled = false;

 private:
  FlashOpResult finish() {
    if (!async) return FlashOpResult::kDone;
    pending_ = true;
    polls_left_ = 2;
    return FlashOpResult::kPending;
  }
  bool pending_ = false;
  unsigned polls_left_ = 0;
};

class FixedIncarnation : public ObservationIncarnationSource {
 public:
  bool generate(uint64_t& value) override {
    value = 0x0102030405060708ULL;
    return true;
  }
};

constexpr uint64_t kDevice = 0x0E8ADE7E71531AA3ULL;
const pr::Jobs kTracker{false, true};

PeriodicRecordInputs period(uint32_t n) {
  PeriodicRecordInputs in;
  in.period_duration_seconds = 3600;
  in.location.valid = true;
  in.location.latitude_e7 = 376102718 + int32_t(n);
  in.location.longitude_e7 = 280545028;
  in.location.hdop_x100 = 120;
  in.location.satellites = 7;
  in.location.age_valid = true;
  in.location.age_seconds = 3;
  in.battery_valid = true;
  in.battery_mv = 4100;
  in.battery_state = 1;
  in.health.valid = true;
  return in;
}

// Submits one period and drives the owner until its outcome arrives.
ProductRecordStore::Outcome store(ProductRecordStore& records, uint32_t n) {
  assert(records.submitPeriodic(period(n)) ==
         ProductRecordStore::Submit::kAccepted);
  ProductRecordStore::Outcome outcome;
  for (unsigned i = 0; i < 20000U; ++i) {
    records.poll();
    if (records.takeOutcome(outcome)) return outcome;
  }
  assert(false && "no outcome");
  return outcome;
}

// The stored plaintext carries exactly the store's identity.
void checkStored(const ProductRecordStore& records, uint32_t sequence,
                 uint32_t n) {
  ObservationStore::Record record;
  osf::RecordIdentity identity;
  identity.incarnation = records.store().incarnation();
  identity.sequence = sequence;
  assert(records.store().lookup(identity, record) ==
         ObservationStore::LookupResult::kFound);
  tlp::PeriodicObservationPlaintext decoded{};
  assert(tlp::decodePeriodicPlaintext(record.payload, record.payload_size,
                                      &decoded) ==
         tlp::ProductSecureCodecResult::kOk);
  assert(decoded.record_incarnation == identity.incarnation);
  assert(decoded.record_sequence == sequence);
  assert(decoded.latitude_e7 == 376102718 + int32_t(n));
}

void testStoreAndRemount(bool async) {
  FakeFlash flash;
  flash.async = async;
  FixedIncarnation source;
  {
    ProductRecordStore records(flash, kPages, &source);
    records.begin(kDevice, kTracker);
    assert(records.regionReadable() && records.mounted());
    assert(records.decision() == pr::MountDecision::kMount);
    assert(records.census().erased == kPages);
    for (uint32_t n = 1; n <= 3; ++n) {
      const auto outcome = store(records, n);
      assert(outcome.stored && outcome.sequence == n);
      assert(outcome.incarnation == 0x0102030405060708ULL);
      checkStored(records, n, n);
    }
    assert(records.diagnostics().stored == 3 &&
           records.diagnostics().failed == 0);
    // A second period while the first is still pending is refused.
    assert(records.submitPeriodic(period(9)) ==
           ProductRecordStore::Submit::kAccepted);
    assert(records.submitPeriodic(period(10)) ==
           ProductRecordStore::Submit::kBusy);
    assert(records.diagnostics().dropped_busy == 1);
    ProductRecordStore::Outcome outcome;
    for (unsigned i = 0; i < 1000U && !records.takeOutcome(outcome); ++i)
      records.poll();
    assert(outcome.stored && outcome.sequence == 4);
  }
  flash.async = false;  // reboot: mount runs before SoftDevice
  ProductRecordStore records(flash, kPages, &source);
  records.begin(kDevice, kTracker);
  assert(records.mounted() && records.census().observation == 1);
  flash.async = async;
  const auto outcome = store(records, 5);
  assert(outcome.stored && outcome.sequence == 5);
  checkStored(records, 2, 2);
}


osc::ExactObject protectedFixture(const ProductRecordStore& records,
                                  uint32_t sequence,
                                  uint64_t source = kDevice,
                                  bool custody_requested = true) {
  osc::ExactObject exact{};
  exact.identity.incarnation = records.store().incarnation();
  exact.identity.sequence = sequence;
  exact.record_kind = osf::RecordKind::kPeriodic;
  exact.object_size = tlp::kPeriodicFrameSize;
  tlp::ProductSecureFrame frame{};
  frame.app_family = tlp::kProductAppFamilyPeriodic;
  frame.path_flags = custody_requested ? tlp::kProductPathFlagCustodyRequested : 0U;
  frame.device_id = source;
  frame.key_epoch = 7U;
  frame.security_counter = sequence + 100U;
  frame.ciphertext_len = tlp::kPeriodicPlaintextSize;
  // Structural fixture only: zero ciphertext/tag is deliberately not AEAD proof.
  assert(tlp::encodeProductSecureFrame(frame, exact.object, exact.object_size) ==
         tlp::ProductSecureCodecResult::kOk);
  return exact;
}

void settleExactCache(ProductRecordStore& records,
                      const osc::ExactObject& expected,
                      bool expected_durable) {
  ProductRecordStore::CacheOutcome outcome{};
  assert(!records.takeCacheOutcome(outcome));
  bool completed = false;
  for (unsigned i = 0; i < 10000U; ++i) {
    assert(!records.requestPrepare());
    records.poll();
    // Includes a faulted store with an unconsumed cache failure outcome.
    assert(!records.requestPrepare());
    if (records.takeCacheOutcome(outcome)) {
      completed = true;
      break;
    }
  }
  assert(completed);
  assert(outcome.identity.incarnation == expected.identity.incarnation);
  assert(outcome.identity.sequence == expected.identity.sequence);
  assert(outcome.durable == expected_durable);
  assert(!records.takeCacheOutcome(outcome));  // outcome is delivered once
}

void checkExact(const ProductRecordStore& records,
                const osc::ExactObject& expected, bool present) {
  osc::ExactObject found{};
  assert(records.findCachedExact(expected.identity, found) ==
         (present ? ObservationStore::ControlLookupResult::kFound
                  : ObservationStore::ControlLookupResult::kNone));
  if (present) {
    assert(found.identity.incarnation == expected.identity.incarnation);
    assert(found.identity.sequence == expected.identity.sequence);
    assert(found.record_kind == expected.record_kind);
    assert(found.object_size == expected.object_size);
    assert(memcmp(found.object, expected.object, expected.object_size) == 0);
  }
}

// Fixture construction uses sequential owners, never a mutable escape from
// ProductRecordStore. Existing occurrence/tombstone writes fill journal slots
// without releasing any backing record or changing the exact objects.
std::vector<uint8_t> cacheImage(unsigned active, unsigned used) {
  FakeFlash flash;
  FixedIncarnation source;
  {
    ProductRecordStore records(flash, kPages, &source);
    records.begin(kDevice, kTracker);
    for (unsigned seq = 1; seq <= 5U; ++seq)
      assert(store(records, seq).stored);
    for (unsigned seq = 1; seq <= active; ++seq) {
      const auto exact = protectedFixture(records, seq);
      assert(records.submitExactCache(exact) ==
             ProductRecordStore::CacheSubmit::kStarted);
      settleExactCache(records, exact, true);
    }
  }
  if (used > active) {
    ObservationStore fixture(flash, kPages, &source);
    assert(fixture.begin(kDevice));
    osc::OpenOccurrence occurrence{};
    occurrence.event_type = 1U;
    occurrence.occurrence_id = 7U;
    for (unsigned i = active; i < used; ++i) {
      const auto result = (i - active) % 2U == 0U
          ? fixture.requestPutOpenOccurrence(occurrence)
          : fixture.requestClearOpenOccurrence(occurrence);
      assert(result == ObservationStore::ControlWriteResult::kStarted);
      for (unsigned step = 0; fixture.busy() && step < 1000U; ++step)
        fixture.poll();
      bool ok = false;
      assert(fixture.takeControlWriteResult(ok) && ok);
    }
  }
  return flash.bytes;
}

void checkRecovery(const std::vector<uint8_t>& bytes, unsigned active,
                   const osc::ExactObject& target, bool target_present,
                   bool expected_fault = false) {
  FakeFlash reboot;
  reboot.bytes = bytes;  // A new backend/owner models reset, not begin in-flight.
  FixedIncarnation source;
  ProductRecordStore recovered(reboot, kPages, &source);
  recovered.begin(kDevice, kTracker);
  assert(recovered.mounted());
  assert(recovered.store().faulted() == expected_fault);
  assert(reboot.mutation_calls == 0U);  // recovery itself must be read-only
  if (expected_fault) {
    // A torn first-ever control activation has no older control authority.
    // Preserve SF5C's faulted/read-only policy; do not erase to hide evidence.
    assert(recovered.submitExactCache(target) ==
           ProductRecordStore::CacheSubmit::kNotMounted);
    assert(recovered.submitPeriodic(period(6U)) ==
           ProductRecordStore::Submit::kNotMounted);
    osc::ExactObject ignored;
    assert(recovered.findCachedExact(target.identity, ignored) !=
           ObservationStore::ControlLookupResult::kFound);
    recovered.poll();
    assert(reboot.mutation_calls == 0U && reboot.bytes == bytes);
    return;
  }
  for (unsigned seq = 1; seq <= active; ++seq)
    checkExact(recovered, protectedFixture(recovered, seq), true);
  checkExact(recovered, target, target_present);
  for (unsigned seq = 1; seq <= 5U; ++seq) checkStored(recovered, seq, seq);
  if (target_present) {
    assert(recovered.submitExactCache(target) ==
           ProductRecordStore::CacheSubmit::kAlreadyDurable);
    assert(reboot.mutation_calls == 0U);
  }
  // The existing periodic path resumes interrupted control maintenance.
  assert(store(recovered, 6U).stored);
  for (unsigned seq = 1; seq <= active; ++seq)
    checkExact(recovered, protectedFixture(recovered, seq), true);
  if (!target_present) {
    assert(recovered.submitExactCache(target) ==
           ProductRecordStore::CacheSubmit::kStarted);
    settleExactCache(recovered, target, true);
  }
  checkExact(recovered, target, true);
}

// SF5K3A next gate: ProductRecordStore performs a READ-ONLY exact-object
// preflight; neither an ACK HMAC nor authority is proven by this comparison.
void testCustodyCandidatePreflightReadOnly(bool async) {
  FakeFlash flash;
  flash.async = async;
  FixedIncarnation source;
  osc::ExactObject first{};
  {
    ProductRecordStore records(flash, kPages, &source);
    records.begin(kDevice, kTracker);
    assert(records.mounted());
    assert(store(records, 1U).stored);
    first = protectedFixture(records, 1U);

    // A retained plaintext record is NOT enough: exact protected bytes must
    // have completed durable cache admission and readback.
    assert(!records.custodyCandidateMatchesRetainedExact(first));
    assert(records.submitExactCache(first) ==
           ProductRecordStore::CacheSubmit::kStarted);
    assert(!records.custodyCandidateMatchesRetainedExact(first));
    settleExactCache(records, first, true);

    const auto writes = flash.mutation_calls;
    const auto image = flash.bytes;
    assert(records.custodyCandidateMatchesRetainedExact(first));
    assert(flash.mutation_calls == writes);
    assert(flash.bytes == image);

    osc::ExactObject different = first;
    different.object[40] ^= 1U;
    assert(!records.custodyCandidateMatchesRetainedExact(different));
    different = first;
    ++different.identity.sequence;
    assert(!records.custodyCandidateMatchesRetainedExact(different));
    different = first;
    different.record_kind = osf::RecordKind::kEvent;
    assert(!records.custodyCandidateMatchesRetainedExact(different));
    different = first;
    different.object_size = 0U;
    assert(!records.custodyCandidateMatchesRetainedExact(different));

    // Read faults, an uncertain flash mutation and an in-flight report must
    // never turn an ACK object into a release-eligible record.
    flash.fail_read_at = flash.read_calls + 1U;
    assert(!records.custodyCandidateMatchesRetainedExact(first));
    flash.fail_read_at = 0U;
    flash.unreconciled = true;
    assert(!records.custodyCandidateMatchesRetainedExact(first));
    flash.unreconciled = false;

    assert(records.submitPeriodic(period(2U)) ==
           ProductRecordStore::Submit::kAccepted);
    assert(!records.custodyCandidateMatchesRetainedExact(first));
    ProductRecordStore::Outcome outcome{};
    bool taken = false;
    for (unsigned i = 0U; i < 20000U; ++i) {
      records.poll();
      if (records.takeOutcome(outcome)) { taken = true; break; }
    }
    assert(taken && outcome.stored);
    assert(records.custodyCandidateMatchesRetainedExact(first));
  }

  flash.async = false;
  {
    ProductRecordStore recovered(flash, kPages, &source);
    recovered.begin(kDevice, kTracker);
    assert(recovered.mounted());
    assert(recovered.custodyCandidateMatchesRetainedExact(first));
    assert(flash.bytes.size() == size_t(kPages) * kPage);
  }

  // A durable logical release makes its cached exact protected object
  // unreachable even when old control-page bytes still physically exist.
  {
    ObservationStore release_owner(flash, kPages, &source);
    assert(release_owner.begin(kDevice));
    assert(release_owner.requestRelease(first.identity));
    for (unsigned i = 0U; i < 1000U && release_owner.busy(); ++i)
      release_owner.poll();
    assert(!release_owner.busy());
    bool committed = false;
    assert(release_owner.takeReleaseResult(committed) && committed);
  }
  {
    ProductRecordStore released(flash, kPages, &source);
    released.begin(kDevice, kTracker);
    assert(released.mounted());
    assert(!released.custodyCandidateMatchesRetainedExact(first));
  }
}

// Enumerate at most four retained exact ciphertext objects without ever
// interpreting an ACK, changing a record, or scanning the full logical backlog.
void testCustodySnapshotReadOnly(bool async) {
  FakeFlash flash;
  flash.async = async;
  FixedIncarnation source;
  osc::ExactObject expected[4]{};
  {
    ProductRecordStore product(flash, kPages, &source);
    product.begin(kDevice, kTracker);
    assert(product.mounted());
    for (uint32_t seq = 1U; seq <= 5U; ++seq)
      assert(store(product, seq).stored);

    osc::ExactObject found[osf::kMaxExactCustodyObjects]{};
    uint16_t count = 77U;
    assert(product.snapshotCustodyCandidates(found, count));
    assert(count == 0U);
    for (uint32_t seq = 1U; seq <= 4U; ++seq) {
      expected[seq - 1U] = protectedFixture(product, seq);
      assert(product.submitExactCache(expected[seq - 1U]) ==
             ProductRecordStore::CacheSubmit::kStarted);
      count = 77U;
      found[0].object_size = 42U;
      assert(!product.snapshotCustodyCandidates(found, count));
      assert(count == 77U && found[0].object_size == 42U);
      settleExactCache(product, expected[seq - 1U], true);
      const unsigned writes_before = flash.mutation_calls;
      assert(product.snapshotCustodyCandidates(found, count));
      assert(count == seq);
      assert(flash.mutation_calls == writes_before);
      for (uint32_t i = 0U; i < seq; ++i) {
        assert(found[i].identity.sequence == expected[i].identity.sequence);
        assert(found[i].object_size == expected[i].object_size);
        assert(memcmp(found[i].object, expected[i].object,
                      expected[i].object_size) == 0);
      }
    }
    // Fifth logical record exists, but the *exact protected cache* is only 4.
    const osc::ExactObject fifth = protectedFixture(product, 5U);
    assert(product.submitExactCache(fifth) ==
           ProductRecordStore::CacheSubmit::kNoCapacity);
    count = 77U;
    assert(product.snapshotCustodyCandidates(found, count));
    assert(count == 4U);
    flash.fail_read_at = flash.read_calls + 1U;
    count = 91U;
    found[0].object_size = 43U;
    assert(!product.snapshotCustodyCandidates(found, count));
    assert(count == 91U && found[0].object_size == 43U);
    flash.fail_read_at = 0U;
    flash.unreconciled = true;
    assert(!product.snapshotCustodyCandidates(found, count));
    assert(count == 91U && found[0].object_size == 43U);
    flash.unreconciled = false;
  }

  flash.async = false;
  {
    ProductRecordStore reboot(flash, kPages, &source);
    reboot.begin(kDevice, kTracker);
    assert(reboot.mounted());
    const auto before = flash.mutation_calls;
    osc::ExactObject recovered[osf::kMaxExactCustodyObjects]{};
    uint16_t count = 0U;
    assert(reboot.snapshotCustodyCandidates(recovered, count));
    assert(count == 4U && flash.mutation_calls == before);
    for (uint16_t i = 0U; i < count; ++i)
      assert(memcmp(recovered[i].object, expected[i].object,
                    expected[i].object_size) == 0);
  }

  // Simulate a previously authorized durable release through the test's
  // direct ObservationStore fixture; the snapshot may no longer return it.
  {
    ObservationStore release_owner(flash, kPages, &source);
    assert(release_owner.begin(kDevice));
    assert(release_owner.requestRelease(expected[1].identity));
    for (unsigned n = 0U; n < 1000U && release_owner.busy(); ++n)
      release_owner.poll();
    assert(!release_owner.busy());
    bool success = false;
    assert(release_owner.takeReleaseResult(success) && success);
  }
  {
    ProductRecordStore after_release(flash, kPages, &source);
    after_release.begin(kDevice, kTracker);
    assert(after_release.mounted());
    osc::ExactObject remaining[osf::kMaxExactCustodyObjects]{};
    uint16_t count = 0U;
    assert(after_release.snapshotCustodyCandidates(remaining, count));
    assert(count == 3U);
    assert(remaining[0].identity.sequence == 1U);
    assert(remaining[1].identity.sequence == 3U);
    assert(remaining[2].identity.sequence == 4U);
  }
}

void testExactObjectCacheAndReboot(bool async) {
  FakeFlash flash;
  flash.async = async;
  FixedIncarnation source;
  osc::ExactObject original{};
  {
    ProductRecordStore records(flash, kPages, &source);
    records.begin(kDevice, kTracker);
    assert(records.mounted());
    assert(store(records, 1).stored);
    ObservationStore::Record oldest{};
    assert(records.oldestRetained(oldest) ==
           ObservationStore::LookupResult::kFound);
    assert(oldest.handle.identity.sequence == 1U);
    original = protectedFixture(records, 1U);
    osc::ExactObject found{};
    assert(records.findCachedExact(original.identity, found) ==
           ObservationStore::ControlLookupResult::kNone);
    assert(records.submitExactCache(original) ==
           ProductRecordStore::CacheSubmit::kStarted);
    settleExactCache(records, original, true);
    assert(records.findCachedExact(original.identity, found) ==
           ObservationStore::ControlLookupResult::kFound);
    assert(found.object_size == original.object_size);
    assert(memcmp(found.object, original.object, found.object_size) == 0);
    assert(records.submitExactCache(original) ==
           ProductRecordStore::CacheSubmit::kAlreadyDurable);

    osc::ExactObject conflict = original;
    conflict.object[27] ^= 1U;  // New counter would violate retry immutability.
    assert(records.submitExactCache(conflict) ==
           ProductRecordStore::CacheSubmit::kRejected);
    assert(records.submitExactCache(protectedFixture(records, 1U, 0x1234U)) ==
           ProductRecordStore::CacheSubmit::kRejected);
    assert(records.submitExactCache(protectedFixture(records, 1U, kDevice, false)) ==
           ProductRecordStore::CacheSubmit::kRejected);

    // An uncommitted/uncached record must not become TX eligible.
    assert(store(records, 2).stored);
    assert(records.findCachedExact(protectedFixture(records, 2U).identity, found) ==
           ObservationStore::ControlLookupResult::kNone);
  }
  flash.async = false;  // Boot recovery before enabling async mutations.
  ProductRecordStore remounted(flash, kPages, &source);
  remounted.begin(kDevice, kTracker);
  assert(remounted.mounted());
  flash.async = async;
  osc::ExactObject recovered{};
  assert(remounted.findCachedExact(original.identity, recovered) ==
         ObservationStore::ControlLookupResult::kFound);
  assert(recovered.object_size == original.object_size);
  assert(memcmp(recovered.object, original.object, original.object_size) == 0);
  assert(remounted.submitExactCache(original) ==
         ProductRecordStore::CacheSubmit::kAlreadyDurable);
  assert(remounted.diagnostics().exact_cache_committed == 0U);
}

void testExactCacheJournalCapacity(bool async) {
  FixedIncarnation source;
  // Fresh, one spare slot, and completely full journals: capacity saturation
  // must never cause compaction, eviction, or any flash mutation on retries.
  for (unsigned used : {4U, osf::kControlRecordsPerPage - 1U,
                        osf::kControlRecordsPerPage}) {
    FakeFlash flash;
    flash.bytes = cacheImage(4U, used);
    const auto before = flash.bytes;
    ProductRecordStore records(flash, kPages, &source);
    records.begin(kDevice, kTracker);
    assert(records.mounted());
    flash.async = async;
    const auto fifth = protectedFixture(records, 5U);
    for (unsigned retry = 0; retry < 8U; ++retry) {
      assert(records.submitExactCache(fifth) ==
             ProductRecordStore::CacheSubmit::kNoCapacity);
      records.poll();
      ProductRecordStore::CacheOutcome outcome;
      assert(!records.takeCacheOutcome(outcome));
      for (unsigned seq = 1; seq <= 4U; ++seq) {
        const auto exact = protectedFixture(records, seq);
        checkExact(records, exact, true);
        assert(records.submitExactCache(exact) ==
               ProductRecordStore::CacheSubmit::kAlreadyDurable);
      }
    }
    checkExact(records, fifth, false);
    assert(flash.program_calls == 0U && flash.erase_calls == 0U);
    assert(flash.bytes == before);
    assert(records.store().diagnostics().control_pages_compacted == 0U);
    // Verify all four remain the immutable authorities after reboot as well.
    ProductRecordStore reboot(flash, kPages, &source);
    reboot.begin(kDevice, kTracker);
    for (unsigned seq = 1; seq <= 4U; ++seq)
      checkExact(reboot, protectedFixture(reboot, seq), true);
    checkExact(reboot, fifth, false);
  }

  // Slot exhaustion with only three live objects is different: compact once
  // and admit the fourth, preserving all previous live objects byte-for-byte.
  FakeFlash flash;
  flash.bytes = cacheImage(3U, osf::kControlRecordsPerPage);
  ProductRecordStore records(flash, kPages, &source);
  records.begin(kDevice, kTracker);
  flash.async = async;
  const auto fourth = protectedFixture(records, 4U);
  assert(records.submitExactCache(fourth) ==
         ProductRecordStore::CacheSubmit::kStarted);
  settleExactCache(records, fourth, true);
  assert(records.store().diagnostics().control_pages_compacted == 1U);
  for (unsigned seq = 1; seq <= 4U; ++seq)
    checkExact(records, protectedFixture(records, seq), true);
  const unsigned mutations = flash.mutation_calls;
  assert(records.submitExactCache(protectedFixture(records, 5U)) ==
         ProductRecordStore::CacheSubmit::kNoCapacity);
  assert(flash.mutation_calls == mutations);
}

void testExactCacheFailuresAndResets(unsigned active, unsigned used) {
  const auto baseline = cacheImage(active, used);
  FixedIncarnation source;
  FakeFlash reference;
  reference.bytes = baseline;
  reference.record = true;
  ProductRecordStore successful(reference, kPages, &source);
  successful.begin(kDevice, kTracker);
  const auto target = protectedFixture(successful, active + 1U);
  assert(successful.submitExactCache(target) ==
         ProductRecordStore::CacheSubmit::kStarted);
  settleExactCache(successful, target, true);
  const auto ops = reference.ops;
  assert(ops.size() >= 2U);
  assert(ops.back().data.size() == 4U);  // final exact-object commit

  for (unsigned fault = 1; fault <= ops.size(); ++fault) {
    // Definite mutation failure, and persistent read failure starting at each
    // mutation's verification. Neither may produce a durable owner outcome.
    for (bool read_failure : {false, true}) {
      FakeFlash flash;
      flash.bytes = baseline;
      ProductRecordStore records(flash, kPages, &source);
      records.begin(kDevice, kTracker);
      assert(records.submitExactCache(target) ==
             ProductRecordStore::CacheSubmit::kStarted);
      if (read_failure)
        flash.fail_reads_after_mutation = fault;
      else
        flash.fail_mutation_at = fault;
      settleExactCache(records, target, false);
      assert(flash.mutation_calls == fault);
      assert(records.diagnostics().exact_cache_committed == 0U);
      assert(records.diagnostics().exact_cache_failed == 1U);
      // A commit can have reached flash even though its verification failed.
      checkRecovery(flash.bytes, active, target,
                    read_failure && fault == ops.size());
    }
  }

  // Fail only the owner's final exact/backing readback, after the lower store
  // has reported a committed control write. Lower-layer success is not enough.
  FakeFlash readback;
  readback.bytes = baseline;
  ProductRecordStore records(readback, kPages, &source);
  records.begin(kDevice, kTracker);
  readback.fail_read_at = reference.read_calls;
  assert(records.submitExactCache(target) ==
         ProductRecordStore::CacheSubmit::kStarted);
  settleExactCache(records, target, false);
  assert(records.store().diagnostics().control_writes_committed == 1U);
  assert(readback.read_calls == reference.read_calls);
  checkRecovery(readback.bytes, active, target, true);

  // Replay the actual successful mutation trace. Reset before, midway through,
  // and after every mutation, including maintenance activation and copy steps.
  // Partial commit words are byte-level models, not physical NVMC evidence.
  std::vector<uint8_t> replay = baseline;
  unsigned cuts = 0U;
  for (size_t i = 0; i < ops.size(); ++i) {
    const auto& op = ops[i];
    const size_t size = op.erase ? kPage : op.data.size();
    for (size_t prefix : {size_t(0), size / 2U, size}) {
      auto cut = replay;
      if (op.erase)
        memset(cut.data() + op.offset, 0xFF, prefix);
      else
        for (size_t byte = 0; byte < prefix; ++byte)
          cut[op.offset + byte] &= op.data[byte];
      checkRecovery(cut, active, target,
                    i + 1U == ops.size() && prefix == size,
                    active == 0U && !op.erase &&
                        op.offset == osf::kPageHeaderActiveOffset &&
                        prefix != 0U && prefix != size);
      ++cuts;
    }
    if (op.erase)
      memset(replay.data() + op.offset, 0xFF, size);
    else
      for (size_t byte = 0; byte < size; ++byte)
        replay[op.offset + byte] &= op.data[byte];
  }
  assert(replay == reference.bytes);
  printf("  exact cache active=%u: %u mutation/read faults, %u reset cuts\n",
         active, unsigned(ops.size() * 2U + 1U), cuts);
}

void testExactCacheTornWrite(bool commit) {
  FakeFlash flash;
  flash.bytes = cacheImage(1U, 1U);
  FixedIncarnation source;
  ProductRecordStore records(flash, kPages, &source);
  records.begin(kDevice, kTracker);
  const auto target = protectedFixture(records, 2U);
  assert(records.submitExactCache(target) ==
         ProductRecordStore::CacheSubmit::kStarted);
  flash.fail_mutation_at = commit ? 2U : 1U;
  flash.torn_bytes = commit ? 2U : osf::kControlRecordCommitOffset / 2U;
  settleExactCache(records, target, false);
  checkExact(records, target, false);
  checkRecovery(flash.bytes, 1U, target, false);
}

void testExactCacheLateCompletion(bool commit, bool quarantined) {
  FakeFlash flash;
  flash.bytes = cacheImage(1U, 1U);
  FixedIncarnation source;
  ProductRecordStore records(flash, kPages, &source);
  records.begin(kDevice, kTracker);
  const auto target = protectedFixture(records, 2U);
  assert(records.submitExactCache(target) ==
         ProductRecordStore::CacheSubmit::kStarted);
  if (commit) records.poll();  // complete body synchronously first
  flash.async = true;
  flash.fail_async_completion = true;
  flash.quarantine_on_failure = quarantined;
  records.poll();  // start pending body or commit (bytes are already visible)
  assert(records.store().busy());
  assert(!records.requestPrepare());
  assert(records.submitExactCache(target) == ProductRecordStore::CacheSubmit::kBusy);
  const unsigned mutations = flash.mutation_calls;
  settleExactCache(records, target, false);
  assert(records.store().faulted());
  assert(records.store().diagnostics().unreconciled_mutation_faults == 1U);
  if (quarantined) assert(!records.requestPrepare());
  flash.unreconciled = false;  // late hardware reconciliation cannot clear latch
  for (unsigned poll = 0; poll < 8U; ++poll) records.poll();
  osc::ExactObject ignored;
  assert(records.findCachedExact(target.identity, ignored) ==
         ObservationStore::ControlLookupResult::kReadError);
  assert(records.submitExactCache(target) ==
         ProductRecordStore::CacheSubmit::kNotMounted);
  assert(flash.mutation_calls == mutations);  // no resubmission or erase
  assert(flash.erase_calls == 0U);
  checkRecovery(flash.bytes, 1U, target, commit);
}

void testOwnersAndPrepare() {
  FakeFlash flash;
  FixedIncarnation source;
  {
    ProductRecordStore records(flash, kPages, &source);
    records.begin(kDevice, pr::Jobs{false, false});
    assert(!records.mounted() &&
           records.decision() == pr::MountDecision::kNoOwner);
    assert(records.submitPeriodic(period(1)) ==
           ProductRecordStore::Submit::kNotMounted);
  }
  {
    // A Gateway owns the region for custody; this owner does not mount it.
    ProductRecordStore records(flash, kPages, &source);
    records.begin(kDevice, pr::Jobs{true, true});
    assert(records.owner() == pr::Owner::kCustody && !records.mounted());
  }

  // Leftovers (old images, test markers) are refused until prepared.
  flash.bytes[7U * kPage + 100U] = 0x00;
  flash.bytes[20U * kPage] = 0x53;
  flash.async = true;
  {
    ProductRecordStore records(flash, kPages, &source);
    records.begin(kDevice, kTracker);
    assert(!records.mounted());
    assert(records.decision() == pr::MountDecision::kNeedsPreparation);
    assert(records.census().unknown == 2);
    assert(records.requestPrepare());
    assert(!records.requestPrepare());  // already running
    bool success = false;
    unsigned guard = 0;
    while (!records.takePrepareResult(success) && guard++ < 10000U)
      records.poll();
    assert(success);
    for (uint8_t b : flash.bytes) assert(b == 0xFF);
  }
  flash.async = false;
  ProductRecordStore records(flash, kPages, &source);
  records.begin(kDevice, kTracker);
  assert(records.mounted());
  const auto outcome = store(records, 1);
  assert(outcome.stored && outcome.sequence == 1);
  // A healthy mounted store is never erased by PREPARE.
  assert(!records.requestPrepare());

  // Pages of the other store: refused, and PREPARE refuses too.
  FakeFlash foreign;
  const uint8_t custody_magic[4] = {'O', 'C', 'Q', '1'};
  memcpy(foreign.bytes.data() + 3U * kPage, custody_magic, 4);
  ProductRecordStore other(foreign, kPages, &source);
  other.begin(kDevice, kTracker);
  assert(other.decision() == pr::MountDecision::kForeignStore);
  assert(!other.mounted() && !other.requestPrepare());
}

// Long run through page rotation and control compaction; then a reset at
// every single write in the second half (the write left half done), each
// followed by a remount that must store the next period.
void testFillAndResetEverywhere() {
  FakeFlash flash;
  FixedIncarnation source;
  constexpr uint32_t kRecords = 2000;
  std::vector<uint8_t> start = flash.bytes;
  flash.record = true;
  {
    ProductRecordStore records(flash, kPages, &source);
    records.begin(kDevice, kTracker);
    for (uint32_t n = 1; n <= kRecords; ++n) {
      const auto outcome = store(records, n);
      assert(outcome.stored && outcome.sequence == n);
    }
    assert(records.store().diagnostics().pages_reclaimed > 10U);
    assert(records.store().diagnostics().control_pages_compacted > 0U);
    assert(records.diagnostics().failed == 0U);
  }
  flash.record = false;
  const std::vector<Op> ops = flash.ops;

  FakeFlash replay;
  replay.bytes = start;
  const size_t first_cut = ops.size() / 2;
  unsigned cuts = 0;
  for (size_t k = 0; k < ops.size(); ++k) {
    const Op& op = ops[k];
    if (k >= first_cut) {
      FakeFlash cut;
      cut.bytes = replay.bytes;
      if (!op.erase) {
        const size_t half = (op.data.size() / 2U) & ~size_t(3U);
        for (size_t i = 0; i < half; ++i) cut.bytes[op.offset + i] &= op.data[i];
      }
      ProductRecordStore records(cut, kPages, &source);
      records.begin(kDevice, kTracker);
      assert(records.mounted() && !records.store().faulted());
      const auto outcome = store(records, 1);
      assert(outcome.stored);
      ++cuts;
    }
    if (op.erase)
      memset(replay.bytes.data() + op.offset, 0xFF, kPage);
    else
      for (size_t i = 0; i < op.data.size(); ++i)
        replay.bytes[op.offset + i] &= op.data[i];
  }
  assert(replay.bytes == flash.bytes);
  printf("  reset/torn write at %u write points recovered\n", cuts);
}

}  // namespace

int main(int argc, char** argv) {
  testCustodySnapshotReadOnly(false);
  testCustodySnapshotReadOnly(true);
  testCustodyCandidatePreflightReadOnly(false);
  testCustodyCandidatePreflightReadOnly(true);
  if (argc == 2 && strcmp(argv[1], "--custody-preflight-only") == 0) {
    puts("SF5K3A ProductRecordStore read-only preflight: PASS");
    return 0;
  }
  testStoreAndRemount(false);
  testStoreAndRemount(true);
  testOwnersAndPrepare();
  testExactObjectCacheAndReboot(false);
  testExactObjectCacheAndReboot(true);
  testExactCacheJournalCapacity(false);
  testExactCacheJournalCapacity(true);
  testExactCacheFailuresAndResets(0U, 0U);  // first control-page maintenance
  testExactCacheFailuresAndResets(3U, osf::kControlRecordsPerPage);
  for (bool commit : {false, true}) {
    testExactCacheTornWrite(commit);
    testExactCacheLateCompletion(commit, false);
    testExactCacheLateCompletion(commit, true);
  }
  testFillAndResetEverywhere();
  puts("SF5F ProductRecordStore checks: PASS");
}
