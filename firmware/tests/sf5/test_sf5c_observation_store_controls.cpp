#include <assert.h>
#include <stdint.h>
#include <string.h>

#include <vector>

#include "observation_store.h"

using namespace orun_tlp;
namespace osf = orun_tlp::observation_store_format;
namespace osc = orun_tlp::observation_store_control;

class ControlFakeFlash : public FlashBackend {
 public:
  explicit ControlFakeFlash(uint16_t pages)
      : bytes(size_t(pages) * osf::kPageSize, 0xFF),
        pages_(pages) {}

  bool begin() override { return true; }
  bool hasUnreconciledMutation() const override { return unreconciled; }
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

  std::vector<uint8_t> bytes;
  bool unreconciled = false;
  unsigned program_calls = 0U;
  unsigned erase_calls = 0U;

 private:
  uint16_t pages_;
};

class ControlIncarnation : public ObservationIncarnationSource {
 public:
  bool generate(uint64_t& value) override {
    value = 0xAABBCCDDEEFF0011ULL;
    return true;
  }
};

static void settle(ObservationStore& store, unsigned limit = 1000U) {
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

static void initData(ObservationStore& store) {
  assert(store.requestMaintenance() ==
         ObservationStore::MaintenanceResult::kStarted);
  settle(store);
  bool ok = false;
  assert(store.takeMaintenanceResult(ok) && ok);
}

static ObservationStore::Handle appendPeriodic(
    ObservationStore& store, uint8_t seed) {
  uint8_t payload[osf::kPeriodicPayloadSizeV1];
  for (unsigned i = 0; i < sizeof(payload); ++i)
    payload[i] = static_cast<uint8_t>(seed + i);
  assert(store.requestAppend(osf::RecordKind::kPeriodic, 1U,
                             payload, sizeof(payload)) ==
         ObservationStore::AppendResult::kStarted);
  settle(store);
  bool ok = false;
  ObservationStore::Handle handle;
  assert(store.takeAppendResult(ok, handle) && ok);
  return handle;
}

static void finishControlWrite(ObservationStore& store) {
  settle(store);
  bool ok = false;
  assert(store.takeControlWriteResult(ok) && ok);
}

static osc::ExactObject exact(uint32_t sequence, uint8_t seed) {
  osc::ExactObject value;
  value.identity.incarnation = 0xAABBCCDDEEFF0011ULL;
  value.identity.sequence = sequence;
  value.record_kind = osf::RecordKind::kPeriodic;
  value.object_size = 100U;
  for (unsigned i = 0; i < value.object_size; ++i)
    value.object[i] = static_cast<uint8_t>(seed + i);
  return value;
}

static osc::OpenOccurrence occurrence(uint64_t id) {
  osc::OpenOccurrence value;
  value.event_type = 1U;
  value.reason_code = 0U;
  value.context_kind = 0U;
  value.context_value = 0U;
  value.occurrence_id = id;
  return value;
}

static osc::ResultGuard resultGuard(uint64_t command_id,
                                    uint32_t result_sequence) {
  osc::ResultGuard value;
  value.gateway_device_id = 0x99U;
  value.gateway_policy_floor = 3U;
  value.gateway_grant_generation = 4U;
  value.command_id = command_id;
  value.opcode = 1U;
  for (unsigned i = 0; i < sizeof(value.expected_state_token); ++i)
    value.expected_state_token[i] = static_cast<uint8_t>(i + 1U);
  value.tracking_interval_seconds = 900U;
  value.battery_capacity_mah = 4000U;
  value.result_identity.incarnation = 0xAABBCCDDEEFF0011ULL;
  value.result_identity.sequence = result_sequence;
  return value;
}

static void testExactObjectBoundAndReplacement() {
  ControlFakeFlash flash(6U);
  ControlIncarnation incarnation;
  ObservationStore store(flash, 6U, &incarnation);
  assert(store.begin(0x11U));
  initData(store);
  for (uint8_t seed = 1U; seed <= 5U; ++seed)
    (void)appendPeriodic(store, seed);

  osc::ExactObject first = exact(1U, 10U);
  assert(store.requestPutExactObject(first) ==
         ObservationStore::ControlWriteResult::kNoCapacity);
  initControl(store);

  assert(store.requestPutExactObject(first) ==
         ObservationStore::ControlWriteResult::kStarted);
  finishControlWrite(store);

  osc::ExactObject found;
  assert(store.findExactObject(first.identity, found) ==
         ObservationStore::ControlLookupResult::kFound);
  assert(found.object_size == first.object_size);
  assert(memcmp(found.object, first.object, first.object_size) == 0);

  assert(store.requestPutExactObject(first) ==
         ObservationStore::ControlWriteResult::kAlreadySatisfied);

  osc::ExactObject conflict = first;
  conflict.object[0] ^= 0x55U;
  assert(store.requestPutExactObject(conflict) ==
         ObservationStore::ControlWriteResult::kRejected);

  osc::ExactObject wrong_kind = first;
  wrong_kind.record_kind = osf::RecordKind::kEvent;
  assert(store.requestPutExactObject(wrong_kind) ==
         ObservationStore::ControlWriteResult::kRejected);

  for (uint32_t seq = 2U; seq <= 4U; ++seq) {
    osc::ExactObject value = exact(seq, static_cast<uint8_t>(10U + seq));
    assert(store.requestPutExactObject(value) ==
           ObservationStore::ControlWriteResult::kStarted);
    finishControlWrite(store);
  }
  osc::ExactObject fifth = exact(5U, 30U);
  assert(store.requestPutExactObject(fifth) ==
         ObservationStore::ControlWriteResult::kNoCapacity);

  // An unreleased record still owns the exact durable protected bytes.
  assert(store.requestClearExactObject(first.identity) ==
         ObservationStore::ControlWriteResult::kRejected);
  assert(store.findExactObject(first.identity, found) ==
         ObservationStore::ControlLookupResult::kFound);
  assert(store.requestPutExactObject(conflict) ==
         ObservationStore::ControlWriteResult::kRejected);

  assert(store.requestRelease(first.identity));
  settle(store);
  bool release_ok = false;
  assert(store.takeReleaseResult(release_ok) && release_ok);
  assert(store.findExactObject(first.identity, found) ==
         ObservationStore::ControlLookupResult::kNone);
  assert(store.requestClearExactObject(first.identity) ==
         ObservationStore::ControlWriteResult::kAlreadySatisfied);

  // The released logical record no longer occupies an exact-object cache slot.
  assert(store.requestPutExactObject(fifth) ==
         ObservationStore::ControlWriteResult::kStarted);
  finishControlWrite(store);
  assert(store.findExactObject(fifth.identity, found) ==
         ObservationStore::ControlLookupResult::kFound);
  assert(store.requestPutExactObject(conflict) ==
         ObservationStore::ControlWriteResult::kRejected);
}

static void testOccurrenceAndResultConflictSurviveReboot() {
  ControlFakeFlash flash(6U);
  ControlIncarnation incarnation;
  ObservationStore store(flash, 6U, &incarnation);
  assert(store.begin(0x22U));
  initControl(store);
  initData(store);
  for (uint8_t seed = 1U; seed <= 4U; ++seed)
    (void)appendPeriodic(store, seed);

  osc::OpenOccurrence open = occurrence(100U);
  assert(store.requestPutOpenOccurrence(open) ==
         ObservationStore::ControlWriteResult::kStarted);
  finishControlWrite(store);

  osc::OpenOccurrence wrong = occurrence(101U);
  assert(store.requestPutOpenOccurrence(wrong) ==
         ObservationStore::ControlWriteResult::kRejected);

  osc::ResultGuard guard = resultGuard(77U, 5U);
  assert(store.requestPutResultGuard(guard) ==
         ObservationStore::ControlWriteResult::kStarted);
  finishControlWrite(store);

  osc::ResultGuard conflict = guard;
  conflict.tracking_interval_seconds = 300U;
  assert(store.requestPutResultGuard(conflict) ==
         ObservationStore::ControlWriteResult::kRejected);

  ObservationStore reboot(flash, 6U, &incarnation);
  assert(reboot.begin(0x22U));
  assert(!reboot.faulted());

  osc::OpenOccurrence recovered_open;
  osc::OpenOccurrence key = open;
  key.occurrence_id = 0U;
  assert(reboot.findOpenOccurrence(key, recovered_open) ==
         ObservationStore::ControlLookupResult::kFound);
  assert(recovered_open.occurrence_id == 100U);

  osc::ResultGuard recovered_guard;
  osc::ResultGuard guard_key = guard;
  guard_key.opcode = 0U;
  guard_key.result_identity = osf::RecordIdentity();
  assert(reboot.findResultGuard(guard_key, recovered_guard) ==
         ObservationStore::ControlLookupResult::kFound);
  assert(recovered_guard.result_identity.sequence == 5U);

  assert(reboot.requestClearOpenOccurrence(key) ==
         ObservationStore::ControlWriteResult::kStarted);
  finishControlWrite(reboot);
  assert(reboot.requestPutOpenOccurrence(wrong) ==
         ObservationStore::ControlWriteResult::kStarted);
  finishControlWrite(reboot);
}

static void testResultGuardReservesIdentityAcrossReset() {
  ControlFakeFlash flash(6U);
  ControlIncarnation incarnation;
  ObservationStore store(flash, 6U, &incarnation);
  assert(store.begin(0x44U));
  initControl(store);
  initData(store);

  osf::RecordIdentity next;
  assert(store.peekNextIdentity(next));
  assert(next.sequence == 1U);

  osc::ResultGuard guard = resultGuard(0x1234U, next.sequence);
  assert(store.requestPutResultGuard(guard) ==
         ObservationStore::ControlWriteResult::kStarted);
  finishControlWrite(store);
  assert(store.requestClearResultGuard(guard) ==
         ObservationStore::ControlWriteResult::kRejected);

  uint8_t periodic_payload[osf::kPeriodicPayloadSizeV1];
  memset(periodic_payload, 0x11, sizeof(periodic_payload));
  assert(store.requestAppend(osf::RecordKind::kPeriodic, 1U,
                             periodic_payload, sizeof(periodic_payload)) ==
         ObservationStore::AppendResult::kBusy);

  ObservationStore reboot(flash, 6U, &incarnation);
  assert(reboot.begin(0x44U));
  assert(!reboot.faulted());

  osc::ResultGuard key = guard;
  key.opcode = 0U;
  key.result_identity = osf::RecordIdentity();
  osc::ResultGuard recovered;
  assert(reboot.findResultGuard(key, recovered) ==
         ObservationStore::ControlLookupResult::kFound);
  assert(recovered.result_identity.sequence == 1U);
  assert(reboot.requestClearResultGuard(key) ==
         ObservationStore::ControlWriteResult::kRejected);

  assert(reboot.peekNextIdentity(next));
  assert(next.sequence == 1U);

  uint8_t result_payload[osf::kResultPayloadSizeV1];
  memset(result_payload, 0, sizeof(result_payload));
  result_payload[0] = osf::kProductSchemaV1;
  osf::put64(result_payload + 4U, guard.command_id + 1U);
  // Same reserved record identity must not be consumed by command 0x1235.
  assert(reboot.requestAppend(osf::RecordKind::kResult, 1U,
                              result_payload, sizeof(result_payload)) ==
         ObservationStore::AppendResult::kRejected);
  osf::put64(result_payload + 4U, guard.command_id);
  assert(reboot.requestAppend(osf::RecordKind::kResult, 1U,
                              result_payload, sizeof(result_payload)) ==
         ObservationStore::AppendResult::kStarted);
  settle(reboot);
  bool append_ok = false;
  ObservationStore::Handle result_handle;
  assert(reboot.takeAppendResult(append_ok, result_handle) && append_ok);
  assert(result_handle.identity.sequence == 1U);

  assert(reboot.requestPutResultGuard(guard) ==
         ObservationStore::ControlWriteResult::kAlreadySatisfied);
  assert(reboot.requestClearResultGuard(key) ==
         ObservationStore::ControlWriteResult::kRejected);

  osc::ResultGuard conflict = guard;
  conflict.tracking_interval_seconds = 300U;
  assert(reboot.requestPutResultGuard(conflict) ==
         ObservationStore::ControlWriteResult::kRejected);

  assert(reboot.requestAppend(osf::RecordKind::kPeriodic, 1U,
                              periodic_payload, sizeof(periodic_payload)) ==
         ObservationStore::AppendResult::kStarted);
  settle(reboot);
  assert(reboot.takeAppendResult(append_ok, result_handle) && append_ok);
  assert(result_handle.identity.sequence == 2U);
}

static void testControlCompactionDropsTombstonedHistory() {
  ControlFakeFlash flash(6U);
  ControlIncarnation incarnation;
  ObservationStore store(flash, 6U, &incarnation);
  assert(store.begin(0x33U));
  initControl(store);

  // 14 create/clear cycles consume all 28 slots while leaving no logical
  // open occurrence. Compaction must copy no stale/tombstoned history.
  for (uint64_t id = 1U; id <= 14U; ++id) {
    osc::OpenOccurrence value = occurrence(id);
    assert(store.requestPutOpenOccurrence(value) ==
           ObservationStore::ControlWriteResult::kStarted);
    finishControlWrite(store);
    assert(store.requestClearOpenOccurrence(value) ==
           ObservationStore::ControlWriteResult::kStarted);
    finishControlWrite(store);
  }

  osc::OpenOccurrence next = occurrence(20U);
  assert(store.requestPutOpenOccurrence(next) ==
         ObservationStore::ControlWriteResult::kNoCapacity);

  assert(store.requestControlMaintenance() ==
         ObservationStore::MaintenanceResult::kStarted);
  settle(store);
  bool ok = false;
  assert(store.takeControlMaintenanceResult(ok) && ok);
  assert(store.diagnostics().control_pages_compacted == 1U);

  assert(store.requestPutOpenOccurrence(next) ==
         ObservationStore::ControlWriteResult::kStarted);
  finishControlWrite(store);

  ObservationStore reboot(flash, 6U, &incarnation);
  assert(reboot.begin(0x33U) && !reboot.faulted());
  osc::OpenOccurrence key = next;
  key.occurrence_id = 0U;
  osc::OpenOccurrence found;
  assert(reboot.findOpenOccurrence(key, found) ==
         ObservationStore::ControlLookupResult::kFound);
  assert(found.occurrence_id == 20U);
}

// Interrupt control compaction at every poll boundary. The prior ACTIVE
// journal must remain authoritative until the new page activates last.
static void testControlCompactionResetMatrix() {
  ControlFakeFlash baseline(6U);
  ControlIncarnation incarnation;
  ObservationStore seed(baseline, 6U, &incarnation);
  assert(seed.begin(0x88U));
  initControl(seed);

  osc::OpenOccurrence durable = occurrence(555U);
  assert(seed.requestPutOpenOccurrence(durable) ==
         ObservationStore::ControlWriteResult::kStarted);
  finishControlWrite(seed);

  osc::OpenOccurrence transient = occurrence(1000U);
  transient.event_type = 2U;
  for (uint64_t i = 0; i < 13U; ++i) {
    transient.occurrence_id = 1000U + i;
    assert(seed.requestPutOpenOccurrence(transient) ==
           ObservationStore::ControlWriteResult::kStarted);
    finishControlWrite(seed);
    assert(seed.requestClearOpenOccurrence(transient) ==
           ObservationStore::ControlWriteResult::kStarted);
    finishControlWrite(seed);
  }

  osc::OpenOccurrence key = durable;
  key.occurrence_id = 0U;
  for (unsigned cut = 0U; cut <= 12U; ++cut) {
    ControlFakeFlash flash = baseline;
    ObservationStore attempt(flash, 6U, &incarnation);
    assert(attempt.begin(0x88U) && !attempt.faulted());
    assert(attempt.requestControlMaintenance() ==
           ObservationStore::MaintenanceResult::kStarted);
    for (unsigned step = 0U; step < cut && attempt.busy(); ++step)
      attempt.poll();

    ObservationStore reboot(flash, 6U, &incarnation);
    assert(reboot.begin(0x88U) && !reboot.faulted());
    osc::OpenOccurrence recovered;
    assert(reboot.findOpenOccurrence(key, recovered) ==
           ObservationStore::ControlLookupResult::kFound);
    assert(recovered.occurrence_id == durable.occurrence_id);

    const auto maintenance = reboot.requestControlMaintenance();
    assert(maintenance == ObservationStore::MaintenanceResult::kStarted ||
           maintenance == ObservationStore::MaintenanceResult::kNoWork);
    if (maintenance == ObservationStore::MaintenanceResult::kStarted) {
      settle(reboot);
      bool ok = false;
      assert(reboot.takeControlMaintenanceResult(ok) && ok);
    }

    ObservationStore second_reboot(flash, 6U, &incarnation);
    assert(second_reboot.begin(0x88U) && !second_reboot.faulted());
    assert(second_reboot.findOpenOccurrence(key, recovered) ==
           ObservationStore::ControlLookupResult::kFound);
    assert(recovered.occurrence_id == durable.occurrence_id);
  }
}


static void testTornRetiredControlEraseRecoversWithoutAuthorityRollback() {
  ControlFakeFlash flash(6U);
  ControlIncarnation incarnation;
  ObservationStore store(flash, 6U, &incarnation);
  assert(store.begin(0x91U));
  initControl(store);

  const osc::OpenOccurrence durable = occurrence(0xABCDU);
  assert(store.requestPutOpenOccurrence(durable) ==
         ObservationStore::ControlWriteResult::kStarted);
  finishControlWrite(store);
  osc::OpenOccurrence transient = occurrence(0U);
  transient.event_type = 2U;
  for (uint32_t i = 0U; i < 13U; ++i) {
    transient.occurrence_id = 2000U + i;
    assert(store.requestPutOpenOccurrence(transient) ==
           ObservationStore::ControlWriteResult::kStarted);
    finishControlWrite(store);
    assert(store.requestClearOpenOccurrence(transient) ==
           ObservationStore::ControlWriteResult::kStarted);
    finishControlWrite(store);
  }
  // First compaction activates control page 1, keeping the old page 0
  // as a stale source (same incarnation, generation one less).
  assert(store.requestControlMaintenance() ==
         ObservationStore::MaintenanceResult::kStarted);
  settle(store);
  bool ok = false;
  assert(store.takeControlMaintenanceResult(ok) && ok);
  for (uint32_t i = 0U; i < 13U; ++i) {
    transient.occurrence_id = 3000U + i;
    assert(store.requestPutOpenOccurrence(transient) ==
           ObservationStore::ControlWriteResult::kStarted);
    finishControlWrite(store);
    assert(store.requestClearOpenOccurrence(transient) ==
           ObservationStore::ControlWriteResult::kStarted);
    finishControlWrite(store);
  }

  // An unrelated corruption of the NEWER authority must still fault, rather
  // than resurrecting the old control journal.
  ControlFakeFlash damaged_authority = flash;
  damaged_authority.bytes[osf::kPageSize] ^= 0x01U;
  ObservationStore unsafe(damaged_authority, 6U, &incarnation);
  assert(unsafe.begin(0x91U) && unsafe.faulted());

  assert(store.requestControlMaintenance() ==
         ObservationStore::MaintenanceResult::kStarted);
  osc::OpenOccurrence key = durable;
  key.occurrence_id = 0U;

  // Probe *every byte boundary*, including 1..3 and 45..48 where
  // header generation/CRC can be partially or entirely lost.
  for (size_t prefix = 1U; prefix <= osf::kPageHeaderCommitOffset;
       ++prefix) {
    ControlFakeFlash torn = flash;
    memset(torn.bytes.data(), 0xFF, prefix);
    ObservationStore reboot(torn, 6U, &incarnation);
    assert(reboot.begin(0x91U) && !reboot.faulted());
    osc::OpenOccurrence recovered;
    assert(reboot.findOpenOccurrence(key, recovered) ==
           ObservationStore::ControlLookupResult::kFound);
    assert(recovered.occurrence_id == durable.occurrence_id);
    assert(reboot.requestControlMaintenance() ==
           ObservationStore::MaintenanceResult::kStarted);
    settle(reboot);
    assert(reboot.takeControlMaintenanceResult(ok) && ok);
    ObservationStore second_boot(torn, 6U, &incarnation);
    assert(second_boot.begin(0x91U) && !second_boot.faulted());
    assert(second_boot.findOpenOccurrence(key, recovered) ==
           ObservationStore::ControlLookupResult::kFound);
  }

  // If erase reaches an ACTIVE marker, neither PREPARED status nor old
  // control generation is proven; never guess the surviving authority.
  ControlFakeFlash missing_proof = flash;
  memset(missing_proof.bytes.data(), 0xFF,
         osf::kPageHeaderActiveOffset + 1U);
  ObservationStore ambiguous(missing_proof, 6U, &incarnation);
  assert(ambiguous.begin(0x91U) && ambiguous.faulted());

  // A truly NEWER (g=2) page with new live state must not be ignored just
  // because its first 48 header bytes were erased and g=1 is still valid.
  ControlFakeFlash newer_damaged = flash;
  ObservationStore latest(newer_damaged, 6U, &incarnation);
  assert(latest.begin(0x91U) && !latest.faulted());
  osc::OpenOccurrence newer_fact = occurrence(0xD00DU);
  newer_fact.event_type = 3U;
  assert(latest.requestPutOpenOccurrence(newer_fact) ==
         ObservationStore::ControlWriteResult::kStarted);
  finishControlWrite(latest);
  memset(newer_damaged.bytes.data() + osf::kPageSize, 0xFF,
         osf::kPageHeaderCommitOffset);
  ObservationStore rollback(newer_damaged, 6U, &incarnation);
  assert(rollback.begin(0x91U) && rollback.faulted());

  // Two successive interruptions: first leaves next-generation target
  // PREPARED, reboot, second tears its erase. Its ACTIVE marker is still FF,
  // hence it never carried control authority.
  ControlFakeFlash prepared = flash;
  ObservationStore interrupted(prepared, 6U, &incarnation);
  assert(interrupted.begin(0x91U) && !interrupted.faulted());
  assert(interrupted.requestControlMaintenance() ==
         ObservationStore::MaintenanceResult::kStarted);
  interrupted.poll();  // erase stale g=1 target
  interrupted.poll();  // header body g=3
  interrupted.poll();  // header commit -> PREPARED g=3
  for (size_t prefix = 1U;
       prefix <= osf::kPageHeaderCommitOffset; ++prefix) {
    ControlFakeFlash twice_cut = prepared;
    memset(twice_cut.bytes.data(), 0xFF, prefix);
    ObservationStore recovered(twice_cut, 6U, &incarnation);
    assert(recovered.begin(0x91U) && !recovered.faulted());
    osc::OpenOccurrence found;
    assert(recovered.findOpenOccurrence(key, found) ==
           ObservationStore::ControlLookupResult::kFound);
    assert(recovered.requestControlMaintenance() ==
           ObservationStore::MaintenanceResult::kStarted);
    settle(recovered);
    bool completed = false;
    assert(recovered.takeControlMaintenanceResult(completed) && completed);
    ObservationStore final_boot(twice_cut, 6U, &incarnation);
    assert(final_boot.begin(0x91U) && !final_boot.faulted());
  }
}

static void testDamagedNewestControlAuthorityCannotRollbackAfterGenerationThree() {
  ControlFakeFlash flash(6U);
  ControlIncarnation incarnation;
  ObservationStore store(flash, 6U, &incarnation);
  assert(store.begin(0x94U));
  initControl(store);
  const osc::OpenOccurrence durable = occurrence(0xABCDU);
  assert(store.requestPutOpenOccurrence(durable) ==
         ObservationStore::ControlWriteResult::kStarted);
  finishControlWrite(store);

  osc::OpenOccurrence transient = occurrence(0U);
  transient.event_type = 2U;
  // The first compaction leaves ACTIVE g=1 and ACTIVE g=2.
  for (uint32_t i = 0U; i < 13U; ++i) {
    transient.occurrence_id = 4000U + i;
    assert(store.requestPutOpenOccurrence(transient) ==
           ObservationStore::ControlWriteResult::kStarted);
    finishControlWrite(store);
    assert(store.requestClearOpenOccurrence(transient) ==
           ObservationStore::ControlWriteResult::kStarted);
    finishControlWrite(store);
  }
  assert(store.requestControlMaintenance() ==
         ObservationStore::MaintenanceResult::kStarted);
  settle(store);
  bool ok = false;
  assert(store.takeControlMaintenanceResult(ok) && ok);

  // The second compaction leaves ACTIVE g=2 on page 1 and ACTIVE g=3
  // on page 0. The surviving alternative thus has generation > 1,
  // exercising snapshot matching instead of the early generation guard.
  for (uint32_t i = 0U; i < 13U; ++i) {
    transient.occurrence_id = 5000U + i;
    assert(store.requestPutOpenOccurrence(transient) ==
           ObservationStore::ControlWriteResult::kStarted);
    finishControlWrite(store);
    assert(store.requestClearOpenOccurrence(transient) ==
           ObservationStore::ControlWriteResult::kStarted);
    finishControlWrite(store);
  }
  assert(store.requestControlMaintenance() ==
         ObservationStore::MaintenanceResult::kStarted);
  settle(store);
  assert(store.takeControlMaintenanceResult(ok) && ok);

  const ControlFakeFlash identical = flash;
  for (size_t prefix = 45U; prefix <= 48U; ++prefix) {
    // Both pages still agree on all latest live controls.
    ControlFakeFlash safe = identical;
    memset(safe.bytes.data(), 0xFF, prefix);
    ObservationStore same(safe, 6U, &incarnation);
    assert(same.begin(0x94U) && !same.faulted());
    osc::OpenOccurrence seen;
    assert(same.findOpenOccurrence(durable, seen) ==
           ObservationStore::ControlLookupResult::kFound);
    assert(seen.occurrence_id == durable.occurrence_id);
  }

  osc::OpenOccurrence newest = occurrence(0xD00DU);
  newest.event_type = 3U;
  assert(store.requestPutOpenOccurrence(newest) ==
         ObservationStore::ControlWriteResult::kStarted);
  finishControlWrite(store);
  for (size_t prefix = 45U; prefix <= 48U; ++prefix) {
    ControlFakeFlash lost_header = flash;
    memset(lost_header.bytes.data(), 0xFF, prefix);
    ObservationStore rollback(lost_header, 6U, &incarnation);
    assert(rollback.begin(0x94U) && rollback.faulted());
  }
}

static void testUnreconciledExactObjectCommitCannotBePublished() {
  ControlFakeFlash flash(6U);
  ControlIncarnation incarnation;
  ObservationStore store(flash, 6U, &incarnation);
  assert(store.begin(0x92U));
  initControl(store);
  initData(store);
  (void)appendPeriodic(store, 0x43U);
  const osc::ExactObject value = exact(1U, 0x22U);
  assert(store.requestPutExactObject(value) ==
         ObservationStore::ControlWriteResult::kStarted);
  store.poll();  // body written; commit not submitted yet

  // A matching durable image does not override an unresolved backend
  // mutation: simulate readback of the commit without definitive completion.
  osf::put32(flash.bytes.data() + osf::kPageHeaderSize +
                 osf::kControlRecordCommitOffset, osf::kCommit);
  flash.unreconciled = true;
  store.poll();
  assert(store.faulted());
  assert(store.diagnostics().unreconciled_mutation_faults == 1U);
  bool success = true;
  assert(store.takeControlWriteResult(success) && !success);
  osc::ExactObject readback;
  assert(store.findExactObject(value.identity, readback) ==
         ObservationStore::ControlLookupResult::kReadError);

  ObservationStore unresolved_reboot(flash, 6U, &incarnation);
  assert(!unresolved_reboot.begin(0x92U));
  assert(unresolved_reboot.faulted());
}


static void testEveryPublicReadRejectsUnreconciledMutation() {
  ControlFakeFlash flash(6U);
  ControlIncarnation incarnation;
  ObservationStore store(flash, 6U, &incarnation);
  assert(store.begin(0x93U));
  initControl(store);
  initData(store);
  const ObservationStore::Handle existing = appendPeriodic(store, 0x10U);
  const osc::ExactObject custody = exact(existing.identity.sequence, 0x22U);
  assert(store.requestPutExactObject(custody) ==
         ObservationStore::ControlWriteResult::kStarted);
  finishControlWrite(store);
  const osc::OpenOccurrence open = occurrence(0xABU);
  assert(store.requestPutOpenOccurrence(open) ==
         ObservationStore::ControlWriteResult::kStarted);
  finishControlWrite(store);
  const osc::ResultGuard guard = resultGuard(0xABU, 2U);
  assert(store.requestPutResultGuard(guard) ==
         ObservationStore::ControlWriteResult::kStarted);
  finishControlWrite(store);

  ObservationStore::Record record;
  uint32_t count = 0U;
  osc::ExactObject exact_result;
  osc::OpenOccurrence open_result;
  osc::ResultGuard guard_result;
  assert(store.oldestRetained(record) ==
         ObservationStore::LookupResult::kFound);
  assert(store.retainedCount(count) && count == 1U);
  assert(store.findExactObject(custody.identity, exact_result) ==
         ObservationStore::ControlLookupResult::kFound);
  assert(store.findOpenOccurrence(open, open_result) ==
         ObservationStore::ControlLookupResult::kFound);
  assert(store.findResultGuard(guard, guard_result) ==
         ObservationStore::ControlLookupResult::kFound);

  // In this interval the backend knows completion is ambiguous, but the
  // store has not had a chance to poll and set its own fault indicator.
  flash.unreconciled = true;
  assert(store.lookup(existing.identity, record) ==
         ObservationStore::LookupResult::kReadError);
  assert(store.oldestRetained(record) ==
         ObservationStore::LookupResult::kReadError);
  assert(!store.retainedCount(count));
  assert(!store.releasedCount(count));
  assert(store.findExactObject(custody.identity, exact_result) ==
         ObservationStore::ControlLookupResult::kReadError);
  assert(store.findOpenOccurrence(open, open_result) ==
         ObservationStore::ControlLookupResult::kReadError);
  assert(store.findResultGuard(guard, guard_result) ==
         ObservationStore::ControlLookupResult::kReadError);
  osf::RecordIdentity next;
  assert(!store.peekNextIdentity(next));
  assert(store.requestMaintenance() ==
         ObservationStore::MaintenanceResult::kRejected);
  assert(store.requestControlMaintenance() ==
         ObservationStore::MaintenanceResult::kRejected);
  assert(store.requestPutOpenOccurrence(occurrence(0xCDU)) ==
         ObservationStore::ControlWriteResult::kRejected);
}

int main() {
  testControlCompactionResetMatrix();
  testTornRetiredControlEraseRecoversWithoutAuthorityRollback();
  testDamagedNewestControlAuthorityCannotRollbackAfterGenerationThree();
  testUnreconciledExactObjectCommitCannotBePublished();
  testEveryPublicReadRejectsUnreconciledMutation();
  testExactObjectBoundAndReplacement();
  testOccurrenceAndResultConflictSurviveReboot();
  testResultGuardReservesIdentityAcrossReset();
  testControlCompactionDropsTombstonedHistory();
  return 0;
}
