#include <assert.h>
#include <stdint.h>
#include <string.h>

#include <vector>

#include "observation_store.h"

using namespace orun_tlp;
namespace osf = orun_tlp::observation_store_format;
namespace osc = orun_tlp::observation_store_control;

class RotationFakeFlash : public FlashBackend {
 public:
  explicit RotationFakeFlash(uint16_t pages)
      : bytes(size_t(pages) * osf::kPageSize, 0xFF), pages_(pages) {}

  bool begin() override { return true; }

  bool read(uint32_t offset, void* data, size_t size) const override {
    if (data == nullptr || size == 0U ||
        uint64_t(offset) + size > bytes.size())
      return false;
    memcpy(data, bytes.data() + offset, size);
    return true;
  }

  FlashOpResult program(uint32_t offset, const void* data,
                        size_t size) override {
    if (pending_.kind != Pending::kNone || data == nullptr || size == 0U ||
        (offset & 3U) != 0U || (size & 3U) != 0U ||
        uint64_t(offset) + size > bytes.size())
      return FlashOpResult::kFailed;

    const uint8_t* src = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i)
      if (bytes[offset + i] != 0xFFU)
        return FlashOpResult::kFailed;

    ++program_calls;
    if (async_mode) {
      pending_.kind = Pending::kProgram;
      pending_.offset = offset;
      pending_.data.assign(src, src + size);
      pending_.polls_left = async_polls;
      return FlashOpResult::kPending;
    }

    applyProgram(offset, src, size);
    return FlashOpResult::kDone;
  }

  FlashOpResult erasePage(uint32_t page) override {
    if (pending_.kind != Pending::kNone || page >= pages_)
      return FlashOpResult::kFailed;

    ++erase_calls;
    if (async_mode) {
      pending_.kind = Pending::kErase;
      pending_.page = static_cast<uint16_t>(page);
      pending_.polls_left = async_polls;
      return FlashOpResult::kPending;
    }

    applyErase(static_cast<uint16_t>(page));
    return FlashOpResult::kDone;
  }

  FlashOpResult pollPending() override {
    if (pending_.kind == Pending::kNone) return FlashOpResult::kFailed;
    if (pending_.polls_left > 0U) {
      --pending_.polls_left;
      return FlashOpResult::kPending;
    }

    const Pending::Kind kind = pending_.kind;
    const uint32_t offset = pending_.offset;
    const uint16_t page = pending_.page;
    const std::vector<uint8_t> data = pending_.data;
    pending_ = Pending();

    if (kind == Pending::kProgram) {
      applyProgram(offset, data.data(), data.size());
      return FlashOpResult::kDone;
    }
    if (kind == Pending::kErase) {
      applyErase(page);
      return FlashOpResult::kDone;
    }
    return FlashOpResult::kFailed;
  }

  void tearErase(uint16_t page) {
    assert(page < pages_);
    const size_t base = size_t(page) * osf::kPageSize;
    memset(bytes.data() + base, 0xFF, osf::kPageSize / 2U);
  }

  std::vector<uint8_t> bytes;
  bool async_mode = false;
  unsigned async_polls = 1U;
  unsigned program_calls = 0U;
  unsigned erase_calls = 0U;

 private:
  struct Pending {
    enum Kind { kNone, kProgram, kErase } kind = kNone;
    uint32_t offset = 0U;
    uint16_t page = 0U;
    std::vector<uint8_t> data;
    unsigned polls_left = 0U;
  } pending_;

  void applyProgram(uint32_t offset, const uint8_t* data, size_t size) {
    for (size_t i = 0; i < size; ++i) bytes[offset + i] &= data[i];
  }

  void applyErase(uint16_t page) {
    memset(bytes.data() + size_t(page) * osf::kPageSize,
           0xFF, osf::kPageSize);
  }

  uint16_t pages_;
};

class RotationIncarnation : public ObservationIncarnationSource {
 public:
  bool generate(uint64_t& value) override {
    value = kValue;
    return true;
  }
  static constexpr uint64_t kValue = 0x123456789ABCDEF1ULL;
};

static void settle(ObservationStore& store, unsigned limit = 4000U) {
  for (unsigned i = 0; i < limit && store.busy(); ++i) store.poll();
  assert(!store.busy());
}

static void initControl(ObservationStore& store) {
  assert(store.requestControlMaintenance() ==
         ObservationStore::MaintenanceResult::kStarted);
  settle(store);
  bool ok = false;
  assert(store.takeControlMaintenanceResult(ok) && ok);
}

static void prepareData(ObservationStore& store) {
  assert(store.requestMaintenance() ==
         ObservationStore::MaintenanceResult::kStarted);
  settle(store);
  bool ok = false;
  assert(store.takeMaintenanceResult(ok) && ok);
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

static ObservationStore::Handle appendRecord(
    ObservationStore& store, osf::RecordKind kind, uint8_t seed) {
  uint8_t payload[osf::kDataPayloadSize];
  const uint8_t size = payloadSize(kind);
  assert(size != 0U);
  memset(payload, 0, sizeof(payload));
  for (unsigned i = 0; i < size; ++i)
    payload[i] = static_cast<uint8_t>(seed + i);
  if (kind == osf::RecordKind::kResult) {
    // Only mutation RESULT v1 is admitted by the reserved RESULT guard.
    payload[0] = osf::kProductSchemaV1;
    osf::put64(payload + 4U, 0x8888U);
  }
  assert(store.requestAppend(kind, 1U, payload, size) ==
         ObservationStore::AppendResult::kStarted);
  settle(store);
  bool ok = false;
  ObservationStore::Handle handle;
  assert(store.takeAppendResult(ok, handle) && ok);
  return handle;
}

static void releaseRecord(ObservationStore& store,
                          const osf::RecordIdentity& identity) {
  assert(store.requestRelease(identity));
  settle(store);
  bool ok = false;
  assert(store.takeReleaseResult(ok) && ok);
}

static void finishControlWrite(ObservationStore& store) {
  settle(store);
  bool ok = false;
  assert(store.takeControlWriteResult(ok) && ok);
}

static osc::ResultGuard resultGuard();

static RotationFakeFlash makeFullStore(bool mixed_first_page) {
  RotationFakeFlash flash(4U);  // 2 control + 2 data pages.
  RotationIncarnation incarnation;
  ObservationStore store(flash, 4U, &incarnation);
  assert(store.begin(0xCAFEU));
  initControl(store);
  prepareData(store);

  for (uint32_t i = 1U; i <= osf::kDataRecordsPerPage; ++i) {
    osf::RecordKind kind = osf::RecordKind::kPeriodic;
    if (mixed_first_page && i == osf::kDataRecordsPerPage - 1U)
      kind = osf::RecordKind::kEvent;
    if (mixed_first_page && i == osf::kDataRecordsPerPage) {
      kind = osf::RecordKind::kResult;
      const osc::ResultGuard guard = resultGuard();
      assert(store.requestPutResultGuard(guard) ==
             ObservationStore::ControlWriteResult::kStarted);
      finishControlWrite(store);
    }
    (void)appendRecord(store, kind, static_cast<uint8_t>(i));
  }

  prepareData(store);
  for (uint32_t i = 0U; i < osf::kDataRecordsPerPage; ++i)
    (void)appendRecord(store, osf::RecordKind::kPeriodic,
                       static_cast<uint8_t>(80U + i));

  uint32_t retained = 0U;
  assert(store.retainedCount(retained));
  assert(retained == 2U * osf::kDataRecordsPerPage);
  return flash;
}

static osc::ExactObject exactFor(uint32_t sequence) {
  osc::ExactObject value;
  value.identity.incarnation = RotationIncarnation::kValue;
  value.identity.sequence = sequence;
  value.record_kind = osf::RecordKind::kPeriodic;
  value.object_size = 100U;
  for (unsigned i = 0; i < value.object_size; ++i)
    value.object[i] = static_cast<uint8_t>(sequence + i);
  return value;
}

static osc::OpenOccurrence openOccurrence() {
  osc::OpenOccurrence value;
  value.event_type = 1U;
  value.occurrence_id = 0x55AAU;
  return value;
}

static osc::ResultGuard resultGuard() {
  osc::ResultGuard value;
  value.gateway_device_id = 0x44U;
  value.gateway_policy_floor = 3U;
  value.gateway_grant_generation = 7U;
  value.command_id = 0x8888U;
  value.opcode = 1U;
  for (unsigned i = 0; i < sizeof(value.expected_state_token); ++i)
    value.expected_state_token[i] = static_cast<uint8_t>(i + 1U);
  value.tracking_interval_seconds = 900U;
  value.battery_capacity_mah = 4000U;
  value.result_identity.incarnation = RotationIncarnation::kValue;
  value.result_identity.sequence = osf::kDataRecordsPerPage;
  return value;
}

static void testOldestFirstRotationAndDurableGapState() {
  RotationFakeFlash flash = makeFullStore(true);
  RotationIncarnation incarnation;
  ObservationStore store(flash, 4U, &incarnation);
  assert(store.begin(0xCAFEU));
  assert(!store.faulted());

  // Release one PERIODIC and the EVENT on the oldest page. Only unreleased
  // responsibility loss contributes to capacity-loss diagnostics.
  osf::RecordIdentity periodic_release;
  periodic_release.incarnation = RotationIncarnation::kValue;
  periodic_release.sequence = 1U;
  osf::RecordIdentity event_release;
  event_release.incarnation = RotationIncarnation::kValue;
  event_release.sequence = osf::kDataRecordsPerPage - 1U;
  releaseRecord(store, periodic_release);
  releaseRecord(store, event_release);

  osc::ExactObject exact = exactFor(2U);
  assert(store.requestPutExactObject(exact) ==
         ObservationStore::ControlWriteResult::kStarted);
  finishControlWrite(store);

  osc::OpenOccurrence open = openOccurrence();
  assert(store.requestPutOpenOccurrence(open) ==
         ObservationStore::ControlWriteResult::kStarted);
  finishControlWrite(store);

  osc::ResultGuard guard = resultGuard();
  assert(store.requestPutResultGuard(guard) ==
         ObservationStore::ControlWriteResult::kAlreadySatisfied);

  assert(store.requestMaintenance() ==
         ObservationStore::MaintenanceResult::kStarted);
  settle(store);
  bool ok = false;
  assert(store.takeMaintenanceResult(ok) && ok);

  const ObservationStore::Diagnostics& d = store.diagnostics();
  assert(d.pages_reclaimed == 1U);
  assert(d.capacity_lost_total == 40U);
  assert(d.capacity_lost_periodic == 39U);
  assert(d.capacity_lost_event == 0U);
  assert(d.capacity_lost_result == 1U);

  ObservationStore::Record record;
  osf::RecordIdentity old_result;
  old_result.incarnation = RotationIncarnation::kValue;
  old_result.sequence = osf::kDataRecordsPerPage;
  assert(store.lookup(old_result, record) ==
         ObservationStore::LookupResult::kNone);

  osf::RecordIdentity newer;
  newer.incarnation = RotationIncarnation::kValue;
  newer.sequence = osf::kDataRecordsPerPage + 1U;
  assert(store.lookup(newer, record) ==
         ObservationStore::LookupResult::kFound);

  osc::ExactObject cached;
  assert(store.findExactObject(exact.identity, cached) ==
         ObservationStore::ControlLookupResult::kNone);

  osc::OpenOccurrence open_key = open;
  open_key.occurrence_id = 0U;
  osc::OpenOccurrence recovered_open;
  assert(store.findOpenOccurrence(open_key, recovered_open) ==
         ObservationStore::ControlLookupResult::kFound);
  assert(recovered_open.occurrence_id == open.occurrence_id);

  osc::ResultGuard guard_key = guard;
  guard_key.opcode = 0U;
  guard_key.result_identity = osf::RecordIdentity();
  osc::ResultGuard recovered_guard;
  // RESULT guard retention is bounded with the retained RESULT row. Once the
  // historical RESULT is capacity-reclaimed, delegated replay/CAS state—not
  // an unbounded command-id journal—remains authoritative.
  assert(store.findResultGuard(guard_key, recovered_guard) ==
         ObservationStore::ControlLookupResult::kNone);

  osf::RecordIdentity next;
  assert(store.peekNextIdentity(next));
  assert(next.sequence == 2U * osf::kDataRecordsPerPage + 1U);

  const ObservationStore::Handle appended =
      appendRecord(store, osf::RecordKind::kPeriodic, 0xA0U);
  assert(appended.identity.sequence == next.sequence);

  ObservationStore reboot(flash, 4U, &incarnation);
  assert(reboot.begin(0xCAFEU));
  assert(!reboot.faulted());
  assert(reboot.diagnostics().capacity_lost_total == 40U);
  assert(reboot.diagnostics().capacity_lost_periodic == 39U);
  assert(reboot.diagnostics().capacity_lost_result == 1U);
  assert(reboot.findOpenOccurrence(open_key, recovered_open) ==
         ObservationStore::ControlLookupResult::kFound);
  assert(reboot.findResultGuard(guard_key, recovered_guard) ==
         ObservationStore::ControlLookupResult::kNone);
}

static void resumeAndVerify(RotationFakeFlash& flash) {
  RotationIncarnation incarnation;
  ObservationStore reboot(flash, 4U, &incarnation);
  assert(reboot.begin(0xCAFEU));
  assert(!reboot.faulted());

  ObservationStore::MaintenanceResult result = reboot.requestMaintenance();
  if (result == ObservationStore::MaintenanceResult::kControlMaintenanceRequired) {
    assert(reboot.requestControlMaintenance() ==
           ObservationStore::MaintenanceResult::kStarted);
    settle(reboot);
    bool control_ok = false;
    assert(reboot.takeControlMaintenanceResult(control_ok) && control_ok);
    result = reboot.requestMaintenance();
  }
  assert(result == ObservationStore::MaintenanceResult::kStarted);
  settle(reboot);
  bool ok = false;
  assert(reboot.takeMaintenanceResult(ok) && ok);
  assert(reboot.diagnostics().capacity_lost_total ==
         osf::kDataRecordsPerPage);

  osf::RecordIdentity next;
  assert(reboot.peekNextIdentity(next));
  assert(next.sequence == 2U * osf::kDataRecordsPerPage + 1U);
}

static void testReleasedPageReclaimedBeforeRetainedHistory() {
  RotationFakeFlash flash = makeFullStore(false);
  RotationIncarnation incarnation;
  ObservationStore store(flash, 4U, &incarnation);
  assert(store.begin(0xCAFEU));

  for (uint32_t seq = osf::kDataRecordsPerPage + 1U;
       seq <= 2U * osf::kDataRecordsPerPage; ++seq) {
    osf::RecordIdentity identity;
    identity.incarnation = RotationIncarnation::kValue;
    identity.sequence = seq;
    releaseRecord(store, identity);
  }

  assert(store.requestMaintenance() ==
         ObservationStore::MaintenanceResult::kStarted);
  settle(store);
  bool ok = false;
  assert(store.takeMaintenanceResult(ok) && ok);
  assert(store.diagnostics().capacity_lost_total == 0U);

  ObservationStore::Record record;
  osf::RecordIdentity retained_oldest;
  retained_oldest.incarnation = RotationIncarnation::kValue;
  retained_oldest.sequence = 1U;
  assert(store.lookup(retained_oldest, record) ==
         ObservationStore::LookupResult::kFound);
  assert(!record.released);

  osf::RecordIdentity reclaimed_newer;
  reclaimed_newer.incarnation = RotationIncarnation::kValue;
  reclaimed_newer.sequence = osf::kDataRecordsPerPage + 1U;
  assert(store.lookup(reclaimed_newer, record) ==
         ObservationStore::LookupResult::kNone);

  ObservationStore reboot(flash, 4U, &incarnation);
  assert(reboot.begin(0xCAFEU));
  assert(!reboot.faulted());
  assert(reboot.diagnostics().capacity_lost_total == 0U);
  assert(reboot.lookup(retained_oldest, record) ==
         ObservationStore::LookupResult::kFound);

  // The responsibility-free newer page carried sequence 43..84. Reclaiming it
  // must not allow those stable logical identities to be reused after reset.
  osf::RecordIdentity next;
  assert(reboot.peekNextIdentity(next));
  assert(next.sequence == 2U * osf::kDataRecordsPerPage + 1U);
}


static void testNewestReclaimedPageRemainsAppendable() {
  RotationFakeFlash flash = makeFullStore(false);
  RotationIncarnation incarnation;
  ObservationStore store(flash, 4U, &incarnation);
  assert(store.begin(0xCAFEU));
  for (uint32_t sequence = osf::kDataRecordsPerPage + 1U;
       sequence <= 2U * osf::kDataRecordsPerPage; ++sequence) {
    osf::RecordIdentity id;
    id.incarnation = RotationIncarnation::kValue;
    id.sequence = sequence;
    releaseRecord(store, id);
  }
  assert(store.requestMaintenance() ==
         ObservationStore::MaintenanceResult::kStarted);
  settle(store);
  bool ok = false;
  assert(store.takeMaintenanceResult(ok) && ok);
  // Page 3 used to be the newest ACTIVE page; it is now PREPARED.
  const auto appended = appendRecord(store, osf::RecordKind::kPeriodic, 0x90U);
  assert(appended.identity.sequence == 85U);
  assert(appended.handle.page == 3U);
  ObservationStore reboot(flash, 4U, &incarnation);
  assert(reboot.begin(0xCAFEU) && !reboot.faulted());
  ObservationStore::Record found;
  assert(reboot.lookup(appended.identity, found) ==
         ObservationStore::LookupResult::kFound);
}

static void testRetiredHighWaterSurvivesStagedOnlyReclamation() {
  RotationFakeFlash flash = makeFullStore(false);
  RotationIncarnation incarnation;
  ObservationStore store(flash, 4U, &incarnation);
  assert(store.begin(0xCAFEU));
  for (uint32_t sequence = osf::kDataRecordsPerPage + 1U;
       sequence <= 2U * osf::kDataRecordsPerPage; ++sequence) {
    osf::RecordIdentity id;
    id.incarnation = RotationIncarnation::kValue;
    id.sequence = sequence;
    releaseRecord(store, id);
  }
  assert(store.requestMaintenance() ==
         ObservationStore::MaintenanceResult::kStarted);
  settle(store);
  bool ok = false;
  assert(store.takeMaintenanceResult(ok) && ok);

  // Interrupt one append after activation and record body, then deliberately
  // leave the remaining slots staged: no committed sequence exists on page 3.
  uint8_t payload[osf::kPeriodicPayloadSizeV1]{};
  assert(store.requestAppend(osf::RecordKind::kPeriodic, 1U,
                             payload, sizeof(payload)) ==
         ObservationStore::AppendResult::kStarted);
  store.poll();  // activate prepared page
  store.poll();  // stage seq 85 body; do NOT commit
  uint8_t staged[osf::kDataRecordSize]{};
  assert(osf::encodeRecord(osf::RecordKind::kPeriodic, 1U,
                           RotationIncarnation::kValue, 85U, payload,
                           sizeof(payload), staged));
  for (uint16_t slot = 1U; slot < osf::kDataRecordsPerPage; ++slot) {
    const uint32_t offset = 3U * osf::kPageSize +
                            osf::kPageHeaderSize +
                            uint32_t(slot) * osf::kDataRecordSize;
    assert(flash.program(offset, staged, osf::kDataRecordCommitOffset) ==
           FlashOpResult::kDone);
  }

  ObservationStore reboot(flash, 4U, &incarnation);
  assert(reboot.begin(0xCAFEU) && !reboot.faulted());
  osf::RecordIdentity next;
  assert(reboot.peekNextIdentity(next));
  assert(next.sequence == 85U);
  // Page 3 is full but has zero committed/unreleased rows; the second
  // reclamation must NOT reset the high-water from 84 back to 42.
  assert(reboot.requestMaintenance() ==
         ObservationStore::MaintenanceResult::kStarted);
  settle(reboot);
  assert(reboot.takeMaintenanceResult(ok) && ok);
  ObservationStore second_boot(flash, 4U, &incarnation);
  assert(second_boot.begin(0xCAFEU) && !second_boot.faulted());
  assert(second_boot.peekNextIdentity(next));
  assert(next.sequence == 85U);
  const auto admitted =
      appendRecord(second_boot, osf::RecordKind::kPeriodic, 0x55U);
  assert(admitted.identity.sequence == 85U);
}

static void testRotationPowerCutMatrix() {
  const RotationFakeFlash baseline = makeFullStore(false);

  // Each poll below completes one synchronous flash mutation in the fresh
  // rotation state machine:
  // 1 intent body, 2 intent commit, 3 erase, 4 new-header body,
  // 5 new-header commit, 6 completion body.
  for (unsigned cut_after = 1U; cut_after <= 6U; ++cut_after) {
    RotationFakeFlash flash = baseline;
    RotationIncarnation incarnation;
    ObservationStore store(flash, 4U, &incarnation);
    assert(store.begin(0xCAFEU));
    assert(store.requestMaintenance() ==
           ObservationStore::MaintenanceResult::kStarted);
    for (unsigned i = 0U; i < cut_after; ++i) {
      assert(store.busy());
      store.poll();
    }
    resumeAndVerify(flash);
  }

  // Torn/partial erase after the durable intent is also recoverable because
  // the StoreState names the exact page/generation that may be destroyed.
  RotationFakeFlash torn = baseline;
  RotationIncarnation incarnation;
  ObservationStore store(torn, 4U, &incarnation);
  assert(store.begin(0xCAFEU));
  assert(store.requestMaintenance() ==
         ObservationStore::MaintenanceResult::kStarted);
  store.poll();  // intent body
  store.poll();  // intent commit
  torn.tearErase(2U);
  resumeAndVerify(torn);
}

static void testRepeatedRotationSurvivesControlCompaction() {
  RotationFakeFlash flash = makeFullStore(false);
  RotationIncarnation incarnation;
  ObservationStore store(flash, 4U, &incarnation);
  assert(store.begin(0xCAFEU));

  const uint32_t rounds = 16U;
  for (uint32_t round = 1U; round <= rounds; ++round) {
    ObservationStore::MaintenanceResult result = store.requestMaintenance();
    if (result ==
        ObservationStore::MaintenanceResult::kControlMaintenanceRequired) {
      assert(store.requestControlMaintenance() ==
             ObservationStore::MaintenanceResult::kStarted);
      settle(store);
      bool control_ok = false;
      assert(store.takeControlMaintenanceResult(control_ok) && control_ok);
      result = store.requestMaintenance();
    }
    assert(result == ObservationStore::MaintenanceResult::kStarted);
    settle(store);
    bool maintenance_ok = false;
    assert(store.takeMaintenanceResult(maintenance_ok) && maintenance_ok);

    assert(store.diagnostics().capacity_lost_total ==
           round * osf::kDataRecordsPerPage);
    assert(store.diagnostics().capacity_lost_periodic ==
           round * osf::kDataRecordsPerPage);

    for (uint32_t i = 0U; i < osf::kDataRecordsPerPage; ++i)
      (void)appendRecord(store, osf::RecordKind::kPeriodic,
                         static_cast<uint8_t>(round + i));
  }

  assert(store.diagnostics().control_pages_compacted >= 1U);

  ObservationStore reboot(flash, 4U, &incarnation);
  assert(reboot.begin(0xCAFEU));
  assert(!reboot.faulted());
  assert(reboot.diagnostics().capacity_lost_total ==
         rounds * osf::kDataRecordsPerPage);
  assert(reboot.diagnostics().capacity_lost_periodic ==
         rounds * osf::kDataRecordsPerPage);
}

static void testAsyncRotationDoesNotResubmitFlashMutations() {
  RotationFakeFlash flash = makeFullStore(false);
  RotationIncarnation incarnation;
  ObservationStore store(flash, 4U, &incarnation);
  assert(store.begin(0xCAFEU));

  const unsigned before_program = flash.program_calls;
  const unsigned before_erase = flash.erase_calls;
  flash.async_mode = true;
  flash.async_polls = 2U;

  assert(store.requestMaintenance() ==
         ObservationStore::MaintenanceResult::kStarted);
  settle(store, 10000U);
  bool ok = false;
  assert(store.takeMaintenanceResult(ok) && ok);

  // One physical submission per state-machine mutation despite repeated
  // pollPending() passes.
  assert(flash.program_calls == before_program + 6U);
  assert(flash.erase_calls == before_erase + 1U);
  assert(store.diagnostics().capacity_lost_total ==
         osf::kDataRecordsPerPage);
}

static void testTornNeverActiveHeaderIsReclaimedBeforeHistory() {
  RotationFakeFlash flash(4U);
  RotationIncarnation incarnation;
  ObservationStore store(flash, 4U, &incarnation);
  assert(store.begin(0xBEEFU));

  assert(store.requestMaintenance() ==
         ObservationStore::MaintenanceResult::kStarted);
  store.poll();  // header body only; commit is still erased.

  ObservationStore reboot(flash, 4U, &incarnation);
  assert(reboot.begin(0xBEEFU));
  assert(!reboot.faulted());
  assert(reboot.requestMaintenance() ==
         ObservationStore::MaintenanceResult::kStarted);
  settle(reboot);
  bool ok = false;
  assert(reboot.takeMaintenanceResult(ok) && ok);
  assert(reboot.hasPreparedDataPage());
  assert(flash.erase_calls == 1U);
}

int main() {
  testOldestFirstRotationAndDurableGapState();
  testReleasedPageReclaimedBeforeRetainedHistory();
  testNewestReclaimedPageRemainsAppendable();
  testRetiredHighWaterSurvivesStagedOnlyReclamation();
  testRotationPowerCutMatrix();
  testRepeatedRotationSurvivesControlCompaction();
  testAsyncRotationDoesNotResubmitFlashMutations();
  testTornNeverActiveHeaderIsReclaimedBeforeHistory();
  return 0;
}
