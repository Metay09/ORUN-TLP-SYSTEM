#include <assert.h>
#include <stdint.h>
#include <string.h>

#include <vector>

#include "observation_store.h"

using namespace orun_tlp;
namespace osf = orun_tlp::observation_store_format;

class FakeFlash : public FlashBackend {
 public:
  explicit FakeFlash(uint16_t page_count)
      : bytes(size_t(page_count) * osf::kPageSize, 0xFF),
        page_count_(page_count) {}

  bool begin() override { return begin_ok; }

  bool read(uint32_t offset, void* data, size_t size) const override {
    if (data == nullptr || size == 0U ||
        uint64_t(offset) + size > bytes.size())
      return false;
    if (fail_read_count > 0U) {
      --fail_read_count;
      return false;
    }
    memcpy(data, bytes.data() + offset, size);
    return true;
  }

  FlashOpResult program(uint32_t offset, const void* data,
                        size_t size) override {
    ++program_calls;
    if (pending_.kind != Pending::kNone || data == nullptr || size == 0U ||
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
    memset(bytes.data() + size_t(page) * osf::kPageSize,
           0xFF, osf::kPageSize);
    return FlashOpResult::kDone;
  }

  FlashOpResult pollPending() override {
    if (pending_.kind == Pending::kNone) return FlashOpResult::kFailed;
    if (pending_.polls_left > 0U) {
      --pending_.polls_left;
      return FlashOpResult::kPending;
    }
    const uint32_t offset = pending_.offset;
    const std::vector<uint8_t> data = pending_.data;
    pending_ = Pending();
    return applyProgram(offset, data.data(), data.size(), program_calls);
  }

  bool hasUnreconciledMutation() const override { return unreconciled; }

  void setFailAfterApplyProgram(unsigned call) {
    fail_after_apply_program_call = call;
  }
  void setPartialProgram(unsigned call) { partial_program_call = call; }
  void failNextReads(unsigned count) const { fail_read_count = count; }

  std::vector<uint8_t> bytes;
  bool begin_ok = true;
  bool async_mode = false;
  unsigned async_polls = 1U;
  bool unreconciled = false;
  unsigned program_calls = 0U;
  unsigned erase_calls = 0U;
  unsigned fail_after_apply_program_call = 0U;
  unsigned partial_program_call = 0U;
  mutable unsigned fail_read_count = 0U;

 private:
  struct Pending {
    enum Kind { kNone, kProgram } kind = kNone;
    uint32_t offset = 0U;
    std::vector<uint8_t> data;
    unsigned polls_left = 0U;
  } pending_;

  FlashOpResult applyProgram(uint32_t offset, const uint8_t* src,
                             size_t size, unsigned call) {
    if (partial_program_call == call) {
      const size_t partial = size > 1U ? size / 2U : 1U;
      for (size_t i = 0; i < partial; ++i) bytes[offset + i] &= src[i];
      partial_program_call = 0U;
      return FlashOpResult::kFailed;
    }
    for (size_t i = 0; i < size; ++i) bytes[offset + i] &= src[i];
    if (fail_after_apply_program_call == call) {
      fail_after_apply_program_call = 0U;
      return FlashOpResult::kFailed;
    }
    return FlashOpResult::kDone;
  }

  uint16_t page_count_;
};

class FixedIncarnation : public ObservationIncarnationSource {
 public:
  explicit FixedIncarnation(uint64_t value) : value_(value) {}
  bool generate(uint64_t& value) override {
    ++calls;
    value = value_;
    return value != 0U;
  }
  uint64_t value_;
  unsigned calls = 0U;
};

static void settle(ObservationStore& store, unsigned limit = 200U) {
  for (unsigned i = 0; i < limit && store.busy(); ++i) store.poll();
  assert(!store.busy());
}

static void prepareOne(ObservationStore& store) {
  assert(store.requestMaintenance() ==
         ObservationStore::MaintenanceResult::kStarted);
  settle(store);
  bool success = false;
  assert(store.takeMaintenanceResult(success) && success);
  assert(store.hasPreparedDataPage());
}

static uint8_t payloadSize(osf::RecordKind kind) {
  switch (kind) {
    case osf::RecordKind::kPeriodic:
      return osf::kPeriodicPayloadSizeV1;
    case osf::RecordKind::kEvent:
      return osf::kEventPayloadSizeV1;
    case osf::RecordKind::kResult:
      return osf::kResultPayloadSizeV1;
  }
  return 0U;
}

static ObservationStore::Handle appendOne(
    ObservationStore& store, osf::RecordKind kind, uint8_t seed) {
  uint8_t payload[osf::kDataPayloadSize];
  const uint8_t size = payloadSize(kind);
  assert(size != 0U);
  memset(payload, 0, sizeof(payload));
  for (unsigned i = 0; i < size; ++i)
    payload[i] = static_cast<uint8_t>(seed + i);
  assert(store.requestAppend(kind, 1U, payload, size) ==
         ObservationStore::AppendResult::kStarted);
  settle(store);
  bool success = false;
  ObservationStore::Handle handle;
  assert(store.takeAppendResult(success, handle) && success);
  return handle;
}

static void prepareControl(ObservationStore& store) {
  assert(store.requestControlMaintenance() ==
         ObservationStore::MaintenanceResult::kStarted);
  settle(store);
  bool success = false;
  assert(store.takeControlMaintenanceResult(success) && success);
}

static void fillTwoDataPages(ObservationStore& store) {
  prepareOne(store);
  (void)appendOne(store, osf::RecordKind::kPeriodic, 1U);

  assert(store.requestMaintenance() ==
         ObservationStore::MaintenanceResult::kStarted);
  settle(store);
  bool maintenance_ok = false;
  assert(store.takeMaintenanceResult(maintenance_ok) && maintenance_ok);

  const uint32_t total =
      2U * static_cast<uint32_t>(osf::kDataRecordsPerPage);
  for (uint32_t sequence = 2U; sequence <= total; ++sequence)
    (void)appendOne(store, osf::RecordKind::kPeriodic,
                    static_cast<uint8_t>(sequence));
}

static void finishDataMaintenance(ObservationStore& store) {
  for (unsigned pass = 0; pass < 4U; ++pass) {
    const ObservationStore::MaintenanceResult result =
        store.requestMaintenance();
    if (result == ObservationStore::MaintenanceResult::kStarted) {
      settle(store, 1000U);
      bool success = false;
      assert(store.takeMaintenanceResult(success) && success);
      return;
    }
    if (result ==
        ObservationStore::MaintenanceResult::kControlMaintenanceRequired) {
      assert(store.requestControlMaintenance() ==
             ObservationStore::MaintenanceResult::kStarted);
      settle(store, 1000U);
      bool success = false;
      assert(store.takeControlMaintenanceResult(success) && success);
      continue;
    }
    assert(false);
  }
  assert(false);
}

static void testBlankAppendReleaseRecovery() {
  FakeFlash flash(5U);  // 2 control + 3 data pages.
  FixedIncarnation source(0x1122334455667788ULL);
  ObservationStore store(flash, 5U, &source);
  assert(store.begin(0x1234U));
  assert(store.ready() && !store.faulted());
  assert(source.calls == 1U);

  osf::RecordIdentity next;
  assert(store.peekNextIdentity(next));
  assert(next.incarnation == source.value_ && next.sequence == 1U);

  uint8_t payload[osf::kPeriodicPayloadSizeV1];
  memset(payload, 1, sizeof(payload));
  assert(store.requestAppend(osf::RecordKind::kPeriodic, 1U,
                             payload, sizeof(payload)) ==
         ObservationStore::AppendResult::kNoCapacity);

  prepareOne(store);
  const ObservationStore::Handle first =
      appendOne(store, osf::RecordKind::kPeriodic, 10U);
  const ObservationStore::Handle second =
      appendOne(store, osf::RecordKind::kEvent, 20U);
  assert(first.identity.sequence == 1U);
  assert(second.identity.sequence == 2U);

  uint32_t retained = 0;
  uint32_t released = 0;
  assert(store.retainedCount(retained) && retained == 2U);
  assert(store.releasedCount(released) && released == 0U);

  // Selective/non-prefix release: newer record may be released first.
  assert(store.requestRelease(second.identity));
  settle(store);
  bool release_ok = false;
  assert(store.takeReleaseResult(release_ok) && release_ok);
  assert(store.retainedCount(retained) && retained == 1U);
  assert(store.releasedCount(released) && released == 1U);

  ObservationStore reboot(flash, 5U, &source);
  assert(reboot.begin(0x1234U));
  assert(reboot.ready() && !reboot.faulted());
  assert(source.calls == 1U);  // Incarnation recovered, not regenerated.
  assert(reboot.incarnation() == source.value_);

  ObservationStore::Record record;
  assert(reboot.lookup(second.identity, record) ==
         ObservationStore::LookupResult::kFound);
  assert(record.released);
  assert(reboot.oldestRetained(record) ==
         ObservationStore::LookupResult::kFound);
  assert(record.handle.identity.sequence == 1U);

  assert(reboot.peekNextIdentity(next));
  assert(next.sequence == 3U);
}

static void testCommitReportedFailureReconciles() {
  FakeFlash flash(4U);
  FixedIncarnation source(9U);
  ObservationStore store(flash, 4U, &source);
  assert(store.begin(1U));
  prepareOne(store);

  // Append: activation, body, commit. Fail after applying the commit word.
  flash.setFailAfterApplyProgram(flash.program_calls + 3U);
  uint8_t payload[osf::kEventPayloadSizeV1];
  memset(payload, 0x12, sizeof(payload));
  assert(store.requestAppend(osf::RecordKind::kEvent, 1U,
                             payload, sizeof(payload)) ==
         ObservationStore::AppendResult::kStarted);
  settle(store);
  bool success = false;
  ObservationStore::Handle handle;
  assert(store.takeAppendResult(success, handle) && success);

  ObservationStore::Record record;
  assert(store.lookup(handle.identity, record) ==
         ObservationStore::LookupResult::kFound);
  assert(record.kind == osf::RecordKind::kEvent);
}

static void testTornReleaseUsesSecondMarker() {
  FakeFlash flash(4U);
  FixedIncarnation source(10U);
  ObservationStore store(flash, 4U, &source);
  assert(store.begin(2U));
  prepareOne(store);
  const ObservationStore::Handle handle =
      appendOne(store, osf::RecordKind::kEvent, 3U);

  flash.setPartialProgram(flash.program_calls + 1U);
  assert(store.requestRelease(handle.identity));
  settle(store);
  bool success = true;
  assert(store.takeReleaseResult(success) && !success);
  assert(!store.faulted());

  ObservationStore::Record record;
  assert(store.lookup(handle.identity, record) ==
         ObservationStore::LookupResult::kFound);
  assert(!record.released);

  assert(store.requestRelease(handle.identity));
  settle(store);
  success = false;
  assert(store.takeReleaseResult(success) && success);

  ObservationStore reboot(flash, 4U, &source);
  assert(reboot.begin(2U) && !reboot.faulted());
  assert(reboot.lookup(handle.identity, record) ==
         ObservationStore::LookupResult::kFound);
  assert(record.released);
}

static void testDoubleTornReleaseIsExposedFailClosed() {
  FakeFlash flash(4U);
  FixedIncarnation source(0xABCDEFU);
  ObservationStore store(flash, 4U, &source);
  assert(store.begin(0x77U));
  prepareOne(store);
  const ObservationStore::Handle handle =
      appendOne(store, osf::RecordKind::kPeriodic, 9U);

  for (unsigned attempt = 0U; attempt < 2U; ++attempt) {
    flash.setPartialProgram(flash.program_calls + 1U);
    assert(store.requestRelease(handle.identity));
    settle(store);
    bool success = true;
    assert(store.takeReleaseResult(success) && !success);
    assert(!store.faulted());
  }

  ObservationStore::Record record;
  assert(store.lookup(handle.identity, record) ==
         ObservationStore::LookupResult::kFound);
  assert(!record.released);
  assert(record.release_uncertain);
  assert(record.release_marker_exhausted);

  assert(!store.requestRelease(handle.identity));
  assert(store.diagnostics().release_marker_exhausted == 1U);

  ObservationStore reboot(flash, 4U, &source);
  assert(reboot.begin(0x77U) && !reboot.faulted());
  assert(reboot.lookup(handle.identity, record) ==
         ObservationStore::LookupResult::kFound);
  assert(record.release_uncertain);
  assert(record.release_marker_exhausted);
}

static void testAsyncNoResubmit() {
  FakeFlash flash(4U);
  FixedIncarnation source(11U);
  ObservationStore store(flash, 4U, &source);
  assert(store.begin(3U));
  flash.async_mode = true;
  flash.async_polls = 1U;
  prepareOne(store);

  const unsigned before = flash.program_calls;
  (void)appendOne(store, osf::RecordKind::kPeriodic, 5U);
  // Header already prepared; append activation/body/commit are exactly three
  // program submissions despite extra pollPending passes.
  assert(flash.program_calls == before + 3U);
}

static void testUnreconciledBeginFailsClosed() {
  FakeFlash flash(4U);
  flash.unreconciled = true;
  FixedIncarnation source(12U);
  ObservationStore store(flash, 4U, &source);
  assert(!store.begin(4U));
  assert(store.faulted());
  assert(store.diagnostics().unreconciled_mutation_faults == 1U);
}

static void testReadFailureFailsClosed() {
  FakeFlash flash(4U);
  FixedIncarnation source(13U);
  ObservationStore store(flash, 4U, &source);
  assert(store.begin(5U));
  prepareOne(store);
  const ObservationStore::Handle handle =
      appendOne(store, osf::RecordKind::kPeriodic, 6U);

  flash.failNextReads(1U);
  ObservationStore::Record record;
  assert(store.lookup(handle.identity, record) ==
         ObservationStore::LookupResult::kReadError);
}

static void testOldestFirstRotationPersistsCapacityLoss() {
  FakeFlash flash(4U);  // 2 control + 2 data pages.
  FixedIncarnation source(0x445566778899AABBULL);
  ObservationStore store(flash, 4U, &source);
  assert(store.begin(0x55U));
  prepareControl(store);
  fillTwoDataPages(store);

  osf::RecordIdentity first;
  first.incarnation = source.value_;
  first.sequence = 1U;
  assert(store.requestRelease(first));
  settle(store);
  bool release_ok = false;
  assert(store.takeReleaseResult(release_ok) && release_ok);

  // Rotation is required only after the active page is full and no erased
  // data page remains. The oldest page contains seq 1..42; seq 1 was already
  // released, so only 41 records are capacity loss.
  assert(store.requestMaintenance() ==
         ObservationStore::MaintenanceResult::kStarted);
  settle(store, 1000U);
  bool maintenance_ok = false;
  assert(store.takeMaintenanceResult(maintenance_ok) && maintenance_ok);
  assert(store.hasPreparedDataPage());
  assert(store.diagnostics().pages_reclaimed == 1U);
  assert(store.diagnostics().capacity_lost_total ==
         osf::kDataRecordsPerPage - 1U);
  assert(store.diagnostics().capacity_lost_periodic ==
         osf::kDataRecordsPerPage - 1U);
  assert(store.diagnostics().capacity_lost_event == 0U);
  assert(store.diagnostics().capacity_lost_result == 0U);

  ObservationStore::Record record;
  assert(store.lookup(first, record) ==
         ObservationStore::LookupResult::kNone);
  assert(store.oldestRetained(record) ==
         ObservationStore::LookupResult::kFound);
  assert(record.handle.identity.sequence ==
         osf::kDataRecordsPerPage + 1U);

  const ObservationStore::Handle next =
      appendOne(store, osf::RecordKind::kPeriodic, 90U);
  assert(next.identity.sequence ==
         2U * osf::kDataRecordsPerPage + 1U);

  ObservationStore reboot(flash, 4U, &source);
  assert(reboot.begin(0x55U));
  assert(reboot.ready() && !reboot.faulted());
  assert(reboot.diagnostics().capacity_lost_total ==
         osf::kDataRecordsPerPage - 1U);
  assert(reboot.diagnostics().capacity_lost_periodic ==
         osf::kDataRecordsPerPage - 1U);
  osf::RecordIdentity identity;
  assert(reboot.peekNextIdentity(identity));
  assert(identity.sequence ==
         2U * osf::kDataRecordsPerPage + 2U);
}

static void testRotationPowerCutRecoveryMatrix() {
  FakeFlash baseline_flash(4U);
  FixedIncarnation baseline_source(0x1122000011112222ULL);
  ObservationStore baseline(baseline_flash, 4U, &baseline_source);
  assert(baseline.begin(0x66U));
  prepareControl(baseline);
  fillTwoDataPages(baseline);

  const std::vector<uint8_t> image = baseline_flash.bytes;

  // Synchronous rotation has seven durable transitions:
  // intent body, intent commit, erase, page-header body, page-header commit,
  // completion body, completion commit. Reboot after each pre-completion
  // transition and require deterministic recovery.
  for (unsigned cut_after_polls = 1U; cut_after_polls <= 6U;
       ++cut_after_polls) {
    FakeFlash flash(4U);
    flash.bytes = image;
    FixedIncarnation source(0x1122000011112222ULL);

    {
      ObservationStore store(flash, 4U, &source);
      assert(store.begin(0x66U));
      assert(store.requestMaintenance() ==
             ObservationStore::MaintenanceResult::kStarted);
      for (unsigned i = 0; i < cut_after_polls; ++i) {
        assert(store.busy());
        store.poll();
      }
    }  // simulated power cut / reset

    ObservationStore reboot(flash, 4U, &source);
    assert(reboot.begin(0x66U));
    assert(reboot.ready() && !reboot.faulted());
    finishDataMaintenance(reboot);
    assert(reboot.hasPreparedDataPage());
    assert(reboot.diagnostics().capacity_lost_total ==
           osf::kDataRecordsPerPage);
    assert(reboot.diagnostics().capacity_lost_periodic ==
           osf::kDataRecordsPerPage);

    ObservationStore second_reboot(flash, 4U, &source);
    assert(second_reboot.begin(0x66U));
    assert(second_reboot.ready() && !second_reboot.faulted());
    assert(second_reboot.diagnostics().capacity_lost_total ==
           osf::kDataRecordsPerPage);

    ObservationStore::Record oldest;
    assert(second_reboot.oldestRetained(oldest) ==
           ObservationStore::LookupResult::kFound);
    assert(oldest.handle.identity.sequence ==
           osf::kDataRecordsPerPage + 1U);
  }
}

int main() {
  testOldestFirstRotationPersistsCapacityLoss();
  testRotationPowerCutRecoveryMatrix();
  testBlankAppendReleaseRecovery();
  testCommitReportedFailureReconciles();
  testTornReleaseUsesSecondMarker();
  testDoubleTornReleaseIsExposedFailClosed();
  testAsyncNoResubmit();
  testUnreconciledBeginFailsClosed();
  testReadFailureFailsClosed();
  return 0;
}
