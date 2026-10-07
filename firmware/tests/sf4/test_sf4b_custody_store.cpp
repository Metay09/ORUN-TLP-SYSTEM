#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <vector>

#include "custody_store.h"

using namespace orun_tlp;
namespace csf = orun_tlp::custody_store_format;

class FakeFlash : public FlashBackend {
 public:
  explicit FakeFlash(uint16_t page_count)
      : bytes(size_t(page_count) * csf::kPageSize, 0xFF),
        page_count_(page_count) {}

  bool begin() override { return begin_ok; }

  bool read(uint32_t offset, void* data, size_t size) const override {
    if (fail_reads || data == nullptr || size == 0 ||
        uint64_t(offset) + size > bytes.size())
      return false;
    if (fail_read_count > 0) {
      --fail_read_count;
      return false;
    }
    memcpy(data, bytes.data() + offset, size);
    return true;
  }

  FlashOpResult program(uint32_t offset, const void* data, size_t size) override {
    ++program_calls;
    if (pending_.kind != Pending::kNone || data == nullptr || size == 0 ||
        (offset & 3U) != 0U || (size & 3U) != 0U ||
        uint64_t(offset) + size > bytes.size())
      return FlashOpResult::kFailed;

    for (size_t i = 0; i < size; ++i)
      if (bytes[offset + i] != 0xFFU)
        return FlashOpResult::kFailed;

    const uint8_t* src = static_cast<const uint8_t*>(data);
    if (async_mode) {
      pending_.kind = Pending::kProgram;
      pending_.offset = offset;
      pending_.data.assign(src, src + size);
      pending_.polls_left = async_polls;
      return FlashOpResult::kPending;
    }
    return applyProgram(offset, src, size, program_calls);
  }

  FlashOpResult erasePage(uint32_t page) override {
    ++erase_calls;
    if (pending_.kind != Pending::kNone || page >= page_count_)
      return FlashOpResult::kFailed;
    if (async_mode) {
      pending_.kind = Pending::kErase;
      pending_.page = page;
      pending_.polls_left = async_polls;
      return FlashOpResult::kPending;
    }
    return applyErase(page, erase_calls);
  }

  FlashOpResult pollPending() override {
    if (pending_.kind == Pending::kNone) return FlashOpResult::kFailed;
    if (pending_.polls_left > 0) {
      --pending_.polls_left;
      return FlashOpResult::kPending;
    }

    const Pending::Kind kind = pending_.kind;
    const uint32_t offset = pending_.offset;
    const uint32_t page = pending_.page;
    const std::vector<uint8_t> data = pending_.data;
    pending_ = Pending();

    if (kind == Pending::kProgram)
      return applyProgram(offset, data.data(), data.size(), program_calls);
    return applyErase(page, erase_calls);
  }

  bool hasUnreconciledMutation() const override { return unreconciled; }

  void setFailAfterApplyProgram(unsigned call) {
    fail_after_apply_program_call = call;
  }
  void setPartialProgram(unsigned call) { partial_program_call = call; }
  void setPartialErase(unsigned call) { partial_erase_call = call; }
  void failNextReads(unsigned count) const { fail_read_count = count; }

  std::vector<uint8_t> bytes;
  bool begin_ok = true;
  bool fail_reads = false;
  bool async_mode = false;
  unsigned async_polls = 1;
  bool unreconciled = false;
  unsigned program_calls = 0;
  unsigned erase_calls = 0;
  unsigned fail_after_apply_program_call = 0;
  unsigned partial_program_call = 0;
  unsigned partial_erase_call = 0;
  mutable unsigned fail_read_count = 0;

 private:
  struct Pending {
    enum Kind { kNone, kProgram, kErase } kind = kNone;
    uint32_t offset = 0;
    uint32_t page = 0;
    std::vector<uint8_t> data;
    unsigned polls_left = 0;
  } pending_;

  FlashOpResult applyProgram(uint32_t offset, const uint8_t* src, size_t size,
                             unsigned call) {
    if (partial_program_call == call) {
      const size_t partial = size > 1 ? size / 2 : 1;
      for (size_t i = 0; i < partial; ++i) bytes[offset + i] &= src[i];
      partial_program_call = 0;
      return FlashOpResult::kFailed;
    }
    for (size_t i = 0; i < size; ++i) bytes[offset + i] &= src[i];
    if (fail_after_apply_program_call == call) {
      fail_after_apply_program_call = 0;
      return FlashOpResult::kFailed;
    }
    return FlashOpResult::kDone;
  }

  FlashOpResult applyErase(uint32_t page, unsigned call) {
    const size_t start = size_t(page) * csf::kPageSize;
    if (partial_erase_call == call) {
      memset(bytes.data() + start, 0xFF, csf::kPageSize / 2);
      partial_erase_call = 0;
      return FlashOpResult::kFailed;
    }
    memset(bytes.data() + start, 0xFF, csf::kPageSize);
    return FlashOpResult::kDone;
  }

  uint16_t page_count_;
};

static void makeObject(uint8_t seed, uint8_t out[csf::kObjectSize]) {
  for (size_t i = 0; i < csf::kObjectSize; ++i)
    out[i] = static_cast<uint8_t>(seed + i * 7U);
}

static void settle(CustodyStore& store, unsigned limit = 20000) {
  for (unsigned i = 0; i < limit && store.busy(); ++i) store.poll();
  assert(!store.busy());
}

static uint32_t held(const CustodyStore& store) {
  uint32_t count = 0;
  assert(store.heldCount(count));
  return count;
}

static uint32_t handed(const CustodyStore& store) {
  uint32_t count = 0;
  assert(store.handedOffCount(count));
  return count;
}

static void prepareOne(CustodyStore& store) {
  assert(store.requestMaintenance() ==
         CustodyStore::MaintenanceResult::kStarted);
  settle(store);
  bool success = false;
  assert(store.takeMaintenanceResult(success) && success);
  assert(store.hasPreparedPage());
}

static CustodyStore::Handle storeOne(
    CustodyStore& store,
    const uint8_t object[csf::kObjectSize]) {
  assert(store.requestCustody(object, csf::kObjectSize) ==
         CustodyStore::AdmissionResult::kStarted);
  settle(store);
  bool success = false;
  CustodyStore::Handle handle;
  assert(store.takeCustodyResult(success, handle) && success);
  return handle;
}

static void handoffOne(
    CustodyStore& store, const CustodyStore::Handle& handle,
    const uint8_t object[csf::kObjectSize]) {
  assert(store.requestMarkEdgeDurableAccepted(
      handle, object, csf::kObjectSize));
  settle(store);
  bool success = false;
  assert(store.takeHandoffResult(success) && success);
}

struct TwoPageFixture {
  explicit TwoPageFixture(FakeFlash& f, CustodyStore& s)
      : flash(f), store(s) {}

  void buildReclaimableFirstPage() {
    assert(store.begin());
    prepareOne(store);

    page0_objects.clear();
    page0_handles.clear();
    for (uint32_t i = 0; i < csf::kRecordsPerPage; ++i) {
      std::vector<uint8_t> object(csf::kObjectSize);
      makeObject(static_cast<uint8_t>(i + 1U), object.data());
      const CustodyStore::Handle handle = storeOne(store, object.data());
      if (i == 0U) prepareOne(store);
      page0_objects.push_back(object);
      page0_handles.push_back(handle);
    }

    makeObject(180, live);
    live_handle = storeOne(store, live);
    for (size_t i = 0; i < page0_handles.size(); ++i)
      handoffOne(store, page0_handles[i], page0_objects[i].data());

    assert(held(store) == 1U);
    assert(!store.hasPreparedPage());
  }

  FakeFlash& flash;
  CustodyStore& store;
  std::vector<std::vector<uint8_t> > page0_objects;
  std::vector<CustodyStore::Handle> page0_handles;
  uint8_t live[csf::kObjectSize]{};
  CustodyStore::Handle live_handle;
};

static void testFormatV2() {
  static_assert(csf::kPageHeaderSize == 64U, "header");
  static_assert(csf::kRecordSize == 92U, "record");
  static_assert(csf::kRecordsPerPage == 43U, "records/page");
  static_assert(csf::kIntentSlotSize == 24U, "intent slot");
  static_assert(csf::kIntentSlotsPerPage == 3U, "intent slots");
  static_assert(csf::kIntentAreaSize == 72U, "intent area");
  static_assert(csf::kRecordIntentGapSize == 4U, "reserved gap");
  static_assert(csf::kObjectSize == 73U, "HISTORY_SECURE observation");
  static_assert(sizeof(CustodyStore) <= 512U, "bounded host ABI store size");

  uint8_t header[csf::kPageHeaderSize];
  csf::encodePageHeader(7, header);
  csf::PageInspection page;
  assert(csf::inspectPageHeader(header, sizeof(header), page));
  assert(page.evidence == csf::PageEvidence::kPrepared);
  assert(page.generation == 7U);

  csf::put32(header + csf::kPageHeaderActiveOffset, 0U);
  assert(csf::inspectPageHeader(header, sizeof(header), page));
  assert(page.evidence == csf::PageEvidence::kActive);

  memset(header, 0xFF, sizeof(header));
  header[0] = 0x4F;
  header[1] = 0x43;
  header[4] = 0x12;
  assert(csf::inspectPageHeader(header, sizeof(header), page));
  assert(page.evidence == csf::PageEvidence::kStaged);

  csf::encodePageHeader(8, header);
  csf::put16(header + 4, 3U);
  csf::put16(header + 6, static_cast<uint16_t>(~uint16_t(3U)));
  csf::put32(header + csf::kPageStaticCrcOffset,
             csf::crc32(header, csf::kPageStaticCrcOffset));
  assert(csf::inspectPageHeader(header, sizeof(header), page));
  assert(page.evidence == csf::PageEvidence::kUnsupported);

  uint8_t intent[csf::kIntentSlotSize];
  csf::encodeReclaimIntent(3, 6, intent);
  csf::ReclaimInspection reclaim;
  assert(csf::inspectReclaimIntent(intent, sizeof(intent), reclaim));
  assert(reclaim.evidence == csf::ReclaimEvidence::kCommitted);
  csf::put32(intent + csf::kIntentCompleteOffset, 0U);
  assert(csf::inspectReclaimIntent(intent, sizeof(intent), reclaim));
  assert(reclaim.evidence == csf::ReclaimEvidence::kCompleted);

  uint8_t object[csf::kObjectSize];
  makeObject(9, object);
  uint8_t record[csf::kRecordSize];
  csf::encodeRecord(object, sizeof(object), record);
  csf::RecordInspection inspected;
  assert(csf::inspectRecord(record, sizeof(record), inspected));
  assert(inspected.evidence == csf::RecordEvidence::kHeld);
  assert(inspected.next_handoff_slot == 0U);
  record[csf::kRecordHandoff0Offset] = 0x00;
  assert(csf::inspectRecord(record, sizeof(record), inspected));
  assert(inspected.evidence == csf::RecordEvidence::kHeld);
  assert(inspected.handoff_uncertain);
  assert(inspected.next_handoff_slot == 1U);
  csf::put32(record + csf::kRecordHandoff1Offset, 0U);
  assert(csf::inspectRecord(record, sizeof(record), inspected));
  assert(inspected.evidence == csf::RecordEvidence::kHandedOff);
}

static void testCommitBeforeAckDuplicateAndQueries() {
  FakeFlash flash(3);
  CustodyStore store(flash, 3);
  assert(store.begin() && store.ready() && !store.faulted());

  uint8_t a[csf::kObjectSize], b[csf::kObjectSize];
  makeObject(1, a);
  makeObject(2, b);
  assert(store.requestCustody(a, sizeof(a)) ==
         CustodyStore::AdmissionResult::kNoCapacity);

  prepareOne(store);
  assert(store.requestCustody(a, sizeof(a)) ==
         CustodyStore::AdmissionResult::kStarted);
  bool success = false;
  CustodyStore::Handle handle;
  assert(!store.takeCustodyResult(success, handle));
  store.poll();
  assert(!store.takeCustodyResult(success, handle));
  store.poll();
  assert(!store.takeCustodyResult(success, handle));
  store.poll();
  assert(store.takeCustodyResult(success, handle) && success);
  assert(held(store) == 1U);

  const unsigned programs = flash.program_calls;
  CustodyStore::Handle duplicate;
  assert(store.requestCustody(a, sizeof(a), &duplicate) ==
         CustodyStore::AdmissionResult::kDuplicateHeld);
  assert(duplicate.page == handle.page &&
         duplicate.slot == handle.slot &&
         duplicate.page_generation == handle.page_generation);
  assert(flash.program_calls == programs);

  const CustodyStore::Handle hb = storeOne(store, b);
  handoffOne(store, handle, a);
  assert(held(store) == 1U && handed(store) == 1U);
  assert(store.requestCustody(a, sizeof(a), &duplicate) ==
         CustodyStore::AdmissionResult::kDuplicateHandedOff);

  CustodyStore reboot(flash, 3);
  assert(reboot.begin() && !reboot.faulted());
  uint8_t oldest[csf::kObjectSize];
  CustodyStore::Handle oldest_handle;
  assert(reboot.oldestHeld(oldest_handle, oldest) ==
         CustodyStore::HeldLookupResult::kFound);
  assert(memcmp(oldest, b, sizeof(b)) == 0);
  assert(oldest_handle.page == hb.page &&
         oldest_handle.slot == hb.slot);

  flash.failNextReads(1);
  uint32_t count = 99;
  assert(!reboot.heldCount(count));
  flash.failNextReads(1);
  assert(reboot.oldestHeld(oldest_handle, oldest) ==
         CustodyStore::HeldLookupResult::kReadError);
}

static void testAsyncProgramAndErase() {
  FakeFlash flash(2);
  flash.async_mode = true;
  flash.async_polls = 1;
  CustodyStore store(flash, 2);
  assert(store.begin());
  prepareOne(store);

  uint8_t a[csf::kObjectSize];
  makeObject(21, a);
  (void)storeOne(store, a);

  flash.async_mode = false;
  for (uint32_t i = 1; i < csf::kRecordsPerPage; ++i) {
    uint8_t object[csf::kObjectSize];
    makeObject(static_cast<uint8_t>(21U + i), object);
    const CustodyStore::Handle h = storeOne(store, object);
    if (i == 1U) prepareOne(store);
    handoffOne(store, h, object);
  }

  CustodyStore reboot(flash, 2);
  assert(reboot.begin());
  CustodyStore::Handle oldest;
  uint8_t oldest_obj[csf::kObjectSize];
  assert(reboot.oldestHeld(oldest, oldest_obj) ==
         CustodyStore::HeldLookupResult::kFound);
  handoffOne(reboot, oldest, oldest_obj);

  uint8_t live[csf::kObjectSize];
  makeObject(200, live);
  (void)storeOne(reboot, live);
  flash.async_mode = true;
  flash.async_polls = 1;
  assert(reboot.requestMaintenance() ==
         CustodyStore::MaintenanceResult::kStarted);
  settle(reboot);
  bool success = false;
  assert(reboot.takeMaintenanceResult(success) && success);
  assert(reboot.hasPreparedPage());
}

static void testTornRecordAndCommitReportedFailure() {
  FakeFlash flash(2);
  CustodyStore store(flash, 2);
  assert(store.begin());
  prepareOne(store);

  uint8_t a[csf::kObjectSize], b[csf::kObjectSize];
  makeObject(31, a);
  makeObject(32, b);
  (void)storeOne(store, a);

  assert(store.requestCustody(b, sizeof(b)) ==
         CustodyStore::AdmissionResult::kStarted);
  store.poll();
  CustodyStore reboot(flash, 2);
  assert(reboot.begin() && !reboot.faulted());
  assert(held(reboot) == 1U);
  assert(reboot.diagnostics().staged_records == 1U);

  uint8_t c[csf::kObjectSize];
  makeObject(33, c);
  flash.setFailAfterApplyProgram(flash.program_calls + 2U);
  assert(reboot.requestCustody(c, sizeof(c)) ==
         CustodyStore::AdmissionResult::kStarted);
  settle(reboot);
  bool success = true;
  CustodyStore::Handle hc;
  assert(reboot.takeCustodyResult(success, hc) && !success);
  assert(!reboot.faulted());
  CustodyStore::Handle dup;
  assert(reboot.requestCustody(c, sizeof(c), &dup) ==
         CustodyStore::AdmissionResult::kDuplicateHeld);
}

static void testTornHandoffUsesSecondWord() {
  FakeFlash flash(2);
  CustodyStore store(flash, 2);
  assert(store.begin());
  prepareOne(store);
  uint8_t a[csf::kObjectSize];
  makeObject(51, a);
  const CustodyStore::Handle h = storeOne(store, a);

  flash.setPartialProgram(flash.program_calls + 1U);
  assert(store.requestMarkEdgeDurableAccepted(h, a, sizeof(a)));
  settle(store);
  bool success = true;
  assert(store.takeHandoffResult(success) && !success);
  assert(!store.faulted());
  assert(held(store) == 1U);

  assert(store.requestMarkEdgeDurableAccepted(h, a, sizeof(a)));
  settle(store);
  success = false;
  assert(store.takeHandoffResult(success) && success);
  assert(handed(store) == 1U);
}

static void testTornHeaderRepairableWithoutReboot() {
  FakeFlash flash(2);
  CustodyStore store(flash, 2);
  assert(store.begin());

  flash.setPartialProgram(flash.program_calls + 1U);
  assert(store.requestMaintenance() ==
         CustodyStore::MaintenanceResult::kStarted);
  settle(store);
  bool success = true;
  assert(store.takeMaintenanceResult(success) && !success);
  assert(!store.faulted());

  assert(store.requestMaintenance() ==
         CustodyStore::MaintenanceResult::kStarted);
  settle(store);
  success = false;
  assert(store.takeMaintenanceResult(success) && success);
  assert(store.hasPreparedPage());
}

static void testMultipleIntentSlotsSurviveTornAttempts() {
  FakeFlash flash(2);
  CustodyStore store(flash, 2);
  TwoPageFixture fixture(flash, store);
  fixture.buildReclaimableFirstPage();

  flash.setPartialProgram(flash.program_calls + 1U);
  assert(store.requestMaintenance() ==
         CustodyStore::MaintenanceResult::kStarted);
  settle(store);
  bool success = true;
  assert(store.takeMaintenanceResult(success) && !success);
  assert(!store.faulted());
  assert(store.diagnostics().reclaim_intent_staged >= 1U);

  assert(store.requestMaintenance() ==
         CustodyStore::MaintenanceResult::kStarted);
  store.poll();
  flash.setPartialProgram(flash.program_calls + 1U);
  settle(store);
  assert(store.takeMaintenanceResult(success) && !success);
  assert(!store.faulted());
  assert(store.diagnostics().reclaim_intent_partial_commits >= 1U);

  assert(store.requestMaintenance() ==
         CustodyStore::MaintenanceResult::kStarted);
  settle(store);
  success = false;
  assert(store.takeMaintenanceResult(success) && success);
  assert(store.hasPreparedPage());
  assert(held(store) == 1U);
}

static void testIntentCommitFailureReconcilesWithoutReboot() {
  FakeFlash flash(2);
  CustodyStore store(flash, 2);
  TwoPageFixture fixture(flash, store);
  fixture.buildReclaimableFirstPage();

  assert(store.requestMaintenance() ==
         CustodyStore::MaintenanceResult::kStarted);
  store.poll();
  flash.setFailAfterApplyProgram(flash.program_calls + 1U);
  settle(store);
  bool success = true;
  assert(store.takeMaintenanceResult(success) && !success);
  assert(!store.faulted());
  assert(store.diagnostics().reclaim_intent_recoveries == 1U);

  assert(store.requestMaintenance() ==
         CustodyStore::MaintenanceResult::kStarted);
  settle(store);
  success = false;
  assert(store.takeMaintenanceResult(success) && success);
  assert(store.hasPreparedPage());
}

static void testPartialEraseRecoveryAndCompletionWord() {
  FakeFlash flash(2);
  CustodyStore store(flash, 2);
  TwoPageFixture fixture(flash, store);
  fixture.buildReclaimableFirstPage();

  assert(store.requestMaintenance() ==
         CustodyStore::MaintenanceResult::kStarted);
  store.poll();
  store.poll();
  flash.setPartialErase(flash.erase_calls + 1U);
  settle(store);
  bool success = true;
  assert(store.takeMaintenanceResult(success) && !success);
  assert(!store.faulted());
  assert(store.diagnostics().reclaim_intent_recoveries == 1U);

  assert(store.requestMaintenance() ==
         CustodyStore::MaintenanceResult::kStarted);
  settle(store);
  success = false;
  assert(store.takeMaintenanceResult(success) && success);
  assert(store.hasPreparedPage());

  CustodyStore reboot(flash, 2);
  assert(reboot.begin() && !reboot.faulted());
  assert(reboot.hasPreparedPage());
  assert(held(reboot) == 1U);
}

static void testStaleIntentCannotEraseReusedHeldPage() {
  FakeFlash flash(2);
  CustodyStore store(flash, 2);
  TwoPageFixture fixture(flash, store);
  fixture.buildReclaimableFirstPage();

  assert(store.requestMaintenance() ==
         CustodyStore::MaintenanceResult::kStarted);
  settle(store);
  bool success = false;
  assert(store.takeMaintenanceResult(success) && success);
  assert(store.hasPreparedPage());

  uint8_t new_object[csf::kObjectSize];
  makeObject(220, new_object);
  const CustodyStore::Handle new_handle = storeOne(store, new_object);
  assert(held(store) == 2U);

  const size_t page_base = size_t(new_handle.page) * csf::kPageSize;
  flash.bytes[page_base + 8] ^= 0x01U;

  const size_t record_offset =
      page_base + csf::kPageHeaderSize +
      size_t(new_handle.slot) * csf::kRecordSize;
  std::vector<uint8_t> before(
      flash.bytes.begin() + record_offset,
      flash.bytes.begin() + record_offset + csf::kRecordSize);

  CustodyStore reboot(flash, 2);
  assert(reboot.begin() && reboot.faulted());
  assert(reboot.requestMaintenance() ==
         CustodyStore::MaintenanceResult::kRejected);
  assert(memcmp(before.data(), flash.bytes.data() + record_offset,
                csf::kRecordSize) == 0);
}

static void testActivationFailureReconcilesWithoutReboot() {
  FakeFlash flash(2);
  CustodyStore store(flash, 2);
  assert(store.begin());
  prepareOne(store);

  uint8_t a[csf::kObjectSize];
  makeObject(70, a);
  flash.setFailAfterApplyProgram(flash.program_calls + 1U);
  assert(store.requestCustody(a, sizeof(a)) ==
         CustodyStore::AdmissionResult::kStarted);
  settle(store);
  bool success = true;
  CustodyStore::Handle h;
  assert(store.takeCustodyResult(success, h) && !success);
  assert(!store.faulted());

  assert(store.requestCustody(a, sizeof(a)) ==
         CustodyStore::AdmissionResult::kStarted);
  settle(store);
  assert(store.takeCustodyResult(success, h) && success);
}

static void testQueueFullNeverErasesHeld() {
  FakeFlash flash(2);
  CustodyStore store(flash, 2);
  assert(store.begin());
  prepareOne(store);

  for (uint32_t i = 0; i < csf::kRecordsPerPage * 2U; ++i) {
    uint8_t object[csf::kObjectSize];
    makeObject(static_cast<uint8_t>(80U + i), object);
    if (i == 1U) prepareOne(store);
    (void)storeOne(store, object);
  }

  assert(held(store) == csf::kRecordsPerPage * 2U);
  const unsigned erases = flash.erase_calls;
  uint8_t extra[csf::kObjectSize];
  makeObject(250, extra);
  assert(store.requestCustody(extra, sizeof(extra)) ==
         CustodyStore::AdmissionResult::kNoCapacity);
  assert(store.requestMaintenance() ==
         CustodyStore::MaintenanceResult::kNoWork);
  assert(flash.erase_calls == erases);
}

static void testReadFailureRejectsDuplicateLookup() {
  FakeFlash flash(2);
  CustodyStore store(flash, 2);
  assert(store.begin());
  prepareOne(store);
  uint8_t a[csf::kObjectSize];
  makeObject(101, a);
  (void)storeOne(store, a);

  const unsigned programs = flash.program_calls;
  flash.failNextReads(1);
  assert(store.requestCustody(a, sizeof(a)) ==
         CustodyStore::AdmissionResult::kRejected);
  assert(flash.program_calls == programs);
}

static void testUnreconciledBeginFailsClosed() {
  FakeFlash flash(2);
  flash.unreconciled = true;
  CustodyStore store(flash, 2);
  assert(!store.begin());
  assert(store.faulted());
  assert(store.diagnostics().unreconciled_mutation_faults == 1U);
}

static void testUnsupportedAndCommittedCorruptionFailClosed() {
  {
    FakeFlash flash(2);
    uint8_t header[csf::kPageHeaderSize];
    csf::encodePageHeader(1, header);
    csf::put16(header + 4, 3U);
    csf::put16(header + 6, static_cast<uint16_t>(~uint16_t(3U)));
    csf::put32(header + csf::kPageStaticCrcOffset,
               csf::crc32(header, csf::kPageStaticCrcOffset));
    memcpy(flash.bytes.data(), header, sizeof(header));
    CustodyStore store(flash, 2);
    assert(store.begin() && store.faulted());
  }
  {
    FakeFlash flash(2);
    CustodyStore store(flash, 2);
    assert(store.begin());
    prepareOne(store);
    uint8_t a[csf::kObjectSize];
    makeObject(200, a);
    const CustodyStore::Handle h = storeOne(store, a);
    const size_t offset =
        size_t(h.page) * csf::kPageSize + csf::kPageHeaderSize +
        size_t(h.slot) * csf::kRecordSize;
    flash.bytes[offset + 10] ^= 0x01U;
    CustodyStore reboot(flash, 2);
    assert(reboot.begin() && reboot.faulted());
  }
}

static void testGenerationBoundaryAndStaleHandle() {
  {
    FakeFlash flash(2);
    uint8_t header[csf::kPageHeaderSize];
    csf::encodePageHeader(UINT64_MAX, header);
    csf::put32(header + csf::kPageHeaderActiveOffset, 0U);
    memcpy(flash.bytes.data(), header, sizeof(header));
    CustodyStore store(flash, 2);
    assert(store.begin() && !store.faulted());
    assert(store.requestMaintenance() ==
           CustodyStore::MaintenanceResult::kRejected);
  }

  FakeFlash flash(2);
  CustodyStore store(flash, 2);
  TwoPageFixture fixture(flash, store);
  fixture.buildReclaimableFirstPage();
  const CustodyStore::Handle stale = fixture.page0_handles[0];
  const std::vector<uint8_t> stale_object = fixture.page0_objects[0];

  assert(store.requestMaintenance() ==
         CustodyStore::MaintenanceResult::kStarted);
  settle(store);
  bool success = false;
  assert(store.takeMaintenanceResult(success) && success);

  uint8_t new_object[csf::kObjectSize];
  makeObject(222, new_object);
  (void)storeOne(store, new_object);
  assert(!store.requestMarkEdgeDurableAccepted(
      stale, stale_object.data(), stale_object.size()));
}

int main() {
  testFormatV2();
  testCommitBeforeAckDuplicateAndQueries();
  testAsyncProgramAndErase();
  testTornRecordAndCommitReportedFailure();
  testTornHandoffUsesSecondWord();
  testTornHeaderRepairableWithoutReboot();
  testMultipleIntentSlotsSurviveTornAttempts();
  testIntentCommitFailureReconcilesWithoutReboot();
  testPartialEraseRecoveryAndCompletionWord();
  testStaleIntentCannotEraseReusedHeldPage();
  testActivationFailureReconcilesWithoutReboot();
  testQueueFullNeverErasesHeld();
  testReadFailureRejectsDuplicateLookup();
  testUnreconciledBeginFailsClosed();
  testUnsupportedAndCommittedCorruptionFailClosed();
  testGenerationBoundaryAndStaleHandle();
  puts("SF4B CustodyStore v2 recovery/power-cut/audit-regression checks: PASS");
  return 0;
}
