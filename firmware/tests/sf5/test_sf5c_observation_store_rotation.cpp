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
    if (data == nullptr || size == 0U || (offset & 3U) != 0U ||
        (size & 3U) != 0U || uint64_t(offset) + size > bytes.size())
      return FlashOpResult::kFailed;
    const uint8_t* src = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i)
      if (bytes[offset + i] != 0xFFU)
        return FlashOpResult::kFailed;
    for (size_t i = 0; i < size; ++i) bytes[offset + i] &= src[i];
    ++program_calls;
    return FlashOpResult::kDone;
  }

  FlashOpResult erasePage(uint32_t page) override {
    if (page >= pages_) return FlashOpResult::kFailed;
    memset(bytes.data() + size_t(page) * osf::kPageSize,
           0xFF, osf::kPageSize);
    ++erase_calls;
    return FlashOpResult::kDone;
  }

  void tearErase(uint16_t page) {
    assert(page < pages_);
    const size_t base = size_t(page) * osf::kPageSize;
    memset(bytes.data() + base, 0xFF, osf::kPageSize / 2U);
  }

  std::vector<uint8_t> bytes;
  unsigned program_calls = 0U;
  unsigned erase_calls = 0U;

 private:
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

static ObservationStore::Handle appendRecord(
    ObservationStore& store, osf::RecordKind kind, uint8_t seed) {
  uint8_t payload[8];
  for (unsigned i = 0; i < sizeof(payload); ++i)
    payload[i] = static_cast<uint8_t>(seed + i);
  assert(store.requestAppend(kind, 1U, payload, sizeof(payload)) ==
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
    if (mixed_first_page && i == osf::kDataRecordsPerPage)
      kind = osf::RecordKind::kResult;
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
  osf::RecordIdentity periodic_release{RotationIncarnation::kValue, 1U};
  osf::RecordIdentity event_release{
      RotationIncarnation::kValue,
      osf::kDataRecordsPerPage - 1U};
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
         ObservationStore::ControlWriteResult::kStarted);
  finishControlWrite(store);

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
  osf::RecordIdentity old_result{
      RotationIncarnation::kValue, osf::kDataRecordsPerPage};
  assert(store.lookup(old_result, record) ==
         ObservationStore::LookupResult::kNone);

  osf::RecordIdentity newer{
      RotationIncarnation::kValue, osf::kDataRecordsPerPage + 1U};
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
  assert(store.findResultGuard(guard_key, recovered_guard) ==
         ObservationStore::ControlLookupResult::kFound);
  assert(recovered_guard.command_id == guard.command_id);

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
         ObservationStore::ControlLookupResult::kFound);
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
  testRotationPowerCutMatrix();
  testTornNeverActiveHeaderIsReclaimedBeforeHistory();
  return 0;
}
