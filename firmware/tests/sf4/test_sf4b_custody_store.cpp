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
    memcpy(data, bytes.data() + offset, size);
    return true;
  }

  FlashOpResult program(uint32_t offset, const void* data, size_t size) override {
    ++program_calls;
    if (pending_.kind != Pending::kNone || data == nullptr || size == 0 ||
        (offset & 3U) != 0U || (size & 3U) != 0U ||
        uint64_t(offset) + size > bytes.size())
      return FlashOpResult::kFailed;
    const uint8_t* src = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i)
      if ((bytes[offset + i] & src[i]) != src[i])
        return FlashOpResult::kFailed;

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
    const auto kind = pending_.kind;
    const uint32_t offset = pending_.offset;
    const uint32_t page = pending_.page;
    const std::vector<uint8_t> data = pending_.data;
    pending_ = Pending{};
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
      return FlashOpResult::kFailed;
    }
    for (size_t i = 0; i < size; ++i) bytes[offset + i] &= src[i];
    if (fail_after_apply_program_call == call) return FlashOpResult::kFailed;
    return FlashOpResult::kDone;
  }

  FlashOpResult applyErase(uint32_t page, unsigned call) {
    const size_t start = size_t(page) * csf::kPageSize;
    if (partial_erase_call == call) {
      memset(bytes.data() + start, 0xFF, csf::kPageSize / 2);
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

static void settle(CustodyStore& store, unsigned limit = 10000) {
  for (unsigned i = 0; i < limit && store.busy(); ++i) store.poll();
  assert(!store.busy());
}

static void prepareOne(CustodyStore& store) {
  assert(store.requestMaintenance() == CustodyStore::MaintenanceResult::kStarted);
  settle(store);
  bool success = false;
  assert(store.takeMaintenanceResult(success) && success);
  assert(store.hasPreparedPage());
}

static CustodyStore::Handle storeOne(CustodyStore& store,
                                     const uint8_t object[csf::kObjectSize]) {
  assert(store.requestCustody(object, csf::kObjectSize) ==
         CustodyStore::AdmissionResult::kStarted);
  settle(store);
  bool success = false;
  CustodyStore::Handle handle;
  assert(store.takeCustodyResult(success, handle) && success);
  return handle;
}

static void handoffOne(CustodyStore& store, const CustodyStore::Handle& handle,
                       const uint8_t object[csf::kObjectSize]) {
  assert(store.requestMarkEdgeDurableAccepted(handle, object, csf::kObjectSize));
  settle(store);
  bool success = false;
  assert(store.takeHandoffResult(success) && success);
}

static void testFormat() {
  static_assert(csf::kPageHeaderSize == 64U, "header size");
  static_assert(csf::kRecordSize == 88U, "record size");
  static_assert(csf::kRecordsPerPage == 45U, "records/page");
  static_assert(csf::kObjectSize == 73U, "current HISTORY_SECURE observation");

  uint8_t header[csf::kPageHeaderSize];
  csf::encodePageHeader(7, header);
  csf::PageInspection page;
  assert(csf::inspectPageHeader(header, sizeof(header), page));
  assert(page.evidence == csf::PageEvidence::kPrepared && page.generation == 7);
  csf::put32(header + csf::kPageHeaderActiveOffset, 0U);
  assert(csf::inspectPageHeader(header, sizeof(header), page));
  assert(page.evidence == csf::PageEvidence::kActive);

  uint8_t intent[csf::kReclaimIntentEnd - csf::kReclaimIntentOffset];
  csf::encodeReclaimIntent(3, 6, intent);
  memcpy(header + csf::kReclaimIntentOffset, intent, sizeof(intent));
  csf::ReclaimInspection reclaim;
  assert(csf::inspectReclaimIntent(header, sizeof(header), reclaim));
  assert(reclaim.evidence == csf::ReclaimEvidence::kCommitted);
  assert(reclaim.target_page == 3 && reclaim.target_generation == 6);

  uint8_t object[csf::kObjectSize];
  makeObject(9, object);
  uint8_t record[csf::kRecordSize];
  csf::encodeRecord(object, sizeof(object), record);
  csf::RecordInspection inspected;
  assert(csf::inspectRecord(record, sizeof(record), inspected));
  assert(inspected.evidence == csf::RecordEvidence::kHeld);
  assert(csf::exactObject(inspected, object, sizeof(object)));

  csf::put32(record + csf::kRecordRetireOffset, 0U);
  assert(csf::inspectRecord(record, sizeof(record), inspected));
  assert(inspected.evidence == csf::RecordEvidence::kHandedOff);

  csf::encodeRecord(object, sizeof(object), record);
  record[csf::kRecordRetireOffset] = 0x00;  // torn retire word
  assert(csf::inspectRecord(record, sizeof(record), inspected));
  assert(inspected.evidence == csf::RecordEvidence::kHeld);
  assert(inspected.retire_uncertain);
}

static void testCommitBeforeAckAndDuplicate() {
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
  store.poll();  // activate prepared page only
  assert(!store.takeCustodyResult(success, handle));
  store.poll();  // body+CRC only
  assert(!store.takeCustodyResult(success, handle));
  store.poll();  // commit + full verification
  assert(store.takeCustodyResult(success, handle) && success);
  assert(store.heldCount() == 1);

  const unsigned programs = flash.program_calls;
  CustodyStore::Handle duplicate;
  assert(store.requestCustody(a, sizeof(a), &duplicate) ==
         CustodyStore::AdmissionResult::kDuplicateHeld);
  assert(duplicate.page == handle.page && duplicate.slot == handle.slot &&
         duplicate.page_generation == handle.page_generation);
  assert(flash.program_calls == programs);

  const auto hb = storeOne(store, b);
  assert(store.heldCount() == 2);
  handoffOne(store, handle, a);
  assert(store.heldCount() == 1 && store.handedOffCount() == 1);
  assert(store.requestCustody(a, sizeof(a), &duplicate) ==
         CustodyStore::AdmissionResult::kDuplicateHandedOff);

  CustodyStore reboot(flash, 3);
  assert(reboot.begin() && !reboot.faulted());
  assert(reboot.heldCount() == 1 && reboot.handedOffCount() == 1);
  uint8_t oldest[csf::kObjectSize];
  CustodyStore::Handle oldest_handle;
  assert(reboot.oldestHeld(oldest_handle, oldest));
  assert(memcmp(oldest, b, sizeof(b)) == 0);
  assert(oldest_handle.page == hb.page && oldest_handle.slot == hb.slot);

  // Re-applying authenticated Edge durable acceptance is idempotent and does
  // not add another flash mutation.
  const unsigned before = flash.program_calls;
  assert(reboot.requestMarkEdgeDurableAccepted(handle, a, sizeof(a)));
  assert(!reboot.busy());
  assert(reboot.takeHandoffResult(success) && success);
  assert(flash.program_calls == before);
}

static void testAsyncCompletionGate() {
  FakeFlash flash(2);
  flash.async_mode = true;
  flash.async_polls = 1;
  CustodyStore store(flash, 2);
  assert(store.begin());
  assert(store.requestMaintenance() == CustodyStore::MaintenanceResult::kStarted);
  bool success = false;
  assert(!store.takeMaintenanceResult(success));
  // Header body submit/pending, pending wait, resolution, then commit follows.
  store.poll();
  assert(store.busy() && !store.takeMaintenanceResult(success));
  settle(store);
  assert(store.takeMaintenanceResult(success) && success);

  uint8_t a[csf::kObjectSize];
  makeObject(21, a);
  assert(store.requestCustody(a, sizeof(a)) ==
         CustodyStore::AdmissionResult::kStarted);
  CustodyStore::Handle handle;
  for (unsigned i = 0; i < 4; ++i) {
    store.poll();
    assert(!store.takeCustodyResult(success, handle));
  }
  settle(store);
  assert(store.takeCustodyResult(success, handle) && success);
}

static void testTornAdmissionRecovery() {
  FakeFlash flash(2);
  CustodyStore store(flash, 2);
  assert(store.begin());
  prepareOne(store);
  uint8_t a[csf::kObjectSize], b[csf::kObjectSize];
  makeObject(31, a);
  makeObject(32, b);
  (void)storeOne(store, a);

  // Power cut after body+CRC, before commit: B is not custody authority.
  assert(store.requestCustody(b, sizeof(b)) ==
         CustodyStore::AdmissionResult::kStarted);
  store.poll();
  assert(store.busy());
  CustodyStore reboot(flash, 2);
  assert(reboot.begin() && !reboot.faulted());
  assert(reboot.heldCount() == 1);
  assert(reboot.diagnostics().staged_records == 1);
  assert(reboot.requestCustody(b, sizeof(b)) ==
         CustodyStore::AdmissionResult::kStarted);
  settle(reboot);
  bool success = false;
  CustodyStore::Handle hb;
  assert(reboot.takeCustodyResult(success, hb) && success);
  assert(reboot.heldCount() == 2);
}

static void testCommitLandedButCallerSawFailure() {
  FakeFlash flash(2);
  CustodyStore store(flash, 2);
  assert(store.begin());
  prepareOne(store);
  uint8_t a[csf::kObjectSize], b[csf::kObjectSize];
  makeObject(41, a);
  makeObject(42, b);
  (void)storeOne(store, a);

  // B body is the next call, B commit the one after it. Physically land the
  // commit but report failure: current attempt must not claim success; reboot
  // may prove the committed object and safely dedupe/re-ACK later.
  flash.setFailAfterApplyProgram(flash.program_calls + 2U);
  assert(store.requestCustody(b, sizeof(b)) ==
         CustodyStore::AdmissionResult::kStarted);
  settle(store);
  bool success = true;
  CustodyStore::Handle hb;
  assert(store.takeCustodyResult(success, hb) && !success);

  CustodyStore reboot(flash, 2);
  assert(reboot.begin() && !reboot.faulted());
  assert(reboot.heldCount() == 2);
  CustodyStore::Handle duplicate;
  assert(reboot.requestCustody(b, sizeof(b), &duplicate) ==
         CustodyStore::AdmissionResult::kDuplicateHeld);
}

static void testTornRetireStaysHeld() {
  FakeFlash flash(2);
  CustodyStore store(flash, 2);
  assert(store.begin());
  prepareOne(store);
  uint8_t a[csf::kObjectSize];
  makeObject(51, a);
  const auto h = storeOne(store, a);

  flash.setPartialProgram(flash.program_calls + 1U);
  assert(store.requestMarkEdgeDurableAccepted(h, a, sizeof(a)));
  settle(store);
  bool success = true;
  assert(store.takeHandoffResult(success) && !success);

  CustodyStore reboot(flash, 2);
  assert(reboot.begin() && !reboot.faulted());
  assert(reboot.heldCount() == 1 && reboot.handedOffCount() == 0);
  assert(reboot.diagnostics().uncertain_retire_markers == 1);
}

static void testQueueFullNeverErasesHeldCustody() {
  FakeFlash flash(2);
  CustodyStore store(flash, 2);
  assert(store.begin());
  prepareOne(store);

  std::vector<std::vector<uint8_t>> objects;
  objects.reserve(csf::kRecordsPerPage * 2U + 1U);
  for (uint32_t i = 0; i < csf::kRecordsPerPage * 2U; ++i) {
    std::vector<uint8_t> object(csf::kObjectSize);
    makeObject(static_cast<uint8_t>(60U + i), object.data());
    if (i == 1U) prepareOne(store);  // reserve page after page 0 became active
    (void)storeOne(store, object.data());
    objects.push_back(object);
  }
  assert(store.heldCount() == csf::kRecordsPerPage * 2U);
  assert(!store.hasPreparedPage());
  const unsigned erases = flash.erase_calls;
  uint8_t extra[csf::kObjectSize];
  makeObject(201, extra);
  assert(store.requestCustody(extra, sizeof(extra)) ==
         CustodyStore::AdmissionResult::kNoCapacity);
  assert(store.requestMaintenance() ==
         CustodyStore::MaintenanceResult::kNoWork);
  assert(flash.erase_calls == erases);
}

static void testReclaimIntentSurvivesErasePowerCut() {
  FakeFlash flash(2);
  CustodyStore store(flash, 2);
  assert(store.begin());
  prepareOne(store);  // page 0 prepared

  std::vector<std::vector<uint8_t>> page0_objects;
  std::vector<CustodyStore::Handle> page0_handles;
  for (uint32_t i = 0; i < csf::kRecordsPerPage; ++i) {
    std::vector<uint8_t> object(csf::kObjectSize);
    makeObject(static_cast<uint8_t>(i + 1U), object.data());
    const auto handle = storeOne(store, object.data());
    if (i == 0U) prepareOne(store);  // page 1 prepared while page 0 active
    page0_objects.push_back(object);
    page0_handles.push_back(handle);
  }

  // Activate page 1 with one live custody record, making page 0 old.
  uint8_t live[csf::kObjectSize];
  makeObject(150, live);
  (void)storeOne(store, live);
  assert(!store.hasPreparedPage());

  // Every page-0 object is durably handed off before reclaim authorization.
  for (size_t i = 0; i < page0_handles.size(); ++i)
    handoffOne(store, page0_handles[i], page0_objects[i].data());
  assert(store.heldCount() == 1);

  assert(store.requestMaintenance() == CustodyStore::MaintenanceResult::kStarted);
  store.poll();  // reclaim-intent body+CRC
  store.poll();  // reclaim-intent commit verified; erase is now authorized
  assert(store.busy());
  flash.setPartialErase(flash.erase_calls + 1U);
  store.poll();  // power-cut model: target erase partially lands then fails
  bool success = true;
  assert(store.takeMaintenanceResult(success) && !success);

  CustodyStore reboot(flash, 2);
  assert(reboot.begin() && !reboot.faulted());
  assert(reboot.heldCount() == 1);  // live page-1 custody remains intact
  assert(reboot.diagnostics().reclaim_intent_recoveries == 1);

  // Recovery resumes the authorized target erase, then creates a prepared
  // page. No held custody is selected for eviction to make room.
  assert(reboot.requestMaintenance() == CustodyStore::MaintenanceResult::kStarted);
  settle(reboot);
  success = false;
  assert(reboot.takeMaintenanceResult(success) && success);
  assert(reboot.hasPreparedPage());
  assert(reboot.heldCount() == 1);
}

static void testCompletedReclaimRebootBeforeActivation() {
  FakeFlash flash(2);
  CustodyStore store(flash, 2);
  assert(store.begin());
  prepareOne(store);  // page 0 prepared

  std::vector<std::vector<uint8_t>> page0_objects;
  std::vector<CustodyStore::Handle> page0_handles;
  for (uint32_t i = 0; i < csf::kRecordsPerPage; ++i) {
    std::vector<uint8_t> object(csf::kObjectSize);
    makeObject(static_cast<uint8_t>(90U + i), object.data());
    const auto handle = storeOne(store, object.data());
    if (i == 0U) prepareOne(store);  // page 1 reserve
    page0_objects.push_back(object);
    page0_handles.push_back(handle);
  }

  uint8_t live[csf::kObjectSize];
  makeObject(180, live);
  (void)storeOne(store, live);  // activates page 1

  for (size_t i = 0; i < page0_handles.size(); ++i)
    handoffOne(store, page0_handles[i], page0_objects[i].data());

  assert(store.requestMaintenance() == CustodyStore::MaintenanceResult::kStarted);
  settle(store);
  bool success = false;
  assert(store.takeMaintenanceResult(success) && success);
  assert(store.hasPreparedPage());
  assert(store.heldCount() == 1);

  // Power loss here is after target erase + PREPARED successor commit, but
  // before that successor is activated by a new admission. The old active page
  // still contains the committed reclaim intent. Recovery must recognize the
  // transaction as complete instead of reapplying that intent to the new page.
  CustodyStore reboot(flash, 2);
  assert(reboot.begin() && reboot.ready() && !reboot.faulted());
  assert(reboot.hasPreparedPage());
  assert(reboot.heldCount() == 1);
  assert(reboot.diagnostics().reclaim_intent_completed_recoveries == 1);

  uint8_t next[csf::kObjectSize];
  makeObject(181, next);
  (void)storeOne(reboot, next);
  assert(reboot.heldCount() == 2);
}

static void testUnsupportedAndCommittedCorruptionFailClosed() {
  {
    FakeFlash flash(2);
    uint8_t header[csf::kPageHeaderSize];
    csf::encodePageHeader(1, header);
    header[4] = 0;
    header[5] = 2;  // unsupported newer version; classifier checks version first
    memcpy(flash.bytes.data(), header, sizeof(header));
    CustodyStore store(flash, 2);
    assert(store.begin() && store.faulted());
    assert(store.requestMaintenance() == CustodyStore::MaintenanceResult::kRejected);
  }
  {
    FakeFlash flash(2);
    CustodyStore store(flash, 2);
    assert(store.begin());
    prepareOne(store);
    uint8_t a[csf::kObjectSize];
    makeObject(200, a);
    const auto h = storeOne(store, a);
    const size_t offset = size_t(h.page) * csf::kPageSize +
                          csf::kPageHeaderSize + size_t(h.slot) * csf::kRecordSize;
    flash.bytes[offset + 10] ^= 0x01;  // commit remains authoritative, CRC fails
    CustodyStore reboot(flash, 2);
    assert(reboot.begin() && reboot.faulted());
    assert(reboot.requestCustody(a, sizeof(a)) ==
           CustodyStore::AdmissionResult::kRejected);
  }
}

int main() {
  testFormat();
  testCommitBeforeAckAndDuplicate();
  testAsyncCompletionGate();
  testTornAdmissionRecovery();
  testCommitLandedButCallerSawFailure();
  testTornRetireStaysHeld();
  testQueueFullNeverErasesHeldCustody();
  testReclaimIntentSurvivesErasePowerCut();
  testCompletedReclaimRebootBeforeActivation();
  testUnsupportedAndCommittedCorruptionFailClosed();
  puts("SF4B CustodyStore format/recovery/power-cut checks: PASS");
  return 0;
}
