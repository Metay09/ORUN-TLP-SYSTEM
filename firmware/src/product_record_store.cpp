#include "product_record_store.h"

#include <string.h>

namespace orun_tlp {

void ProductRecordStore::begin(uint64_t device_id,
                               const product_region::Jobs& jobs) {
  mounted_ = false;
  device_id_ = device_id;
  owner_ = product_region::ownerFor(jobs);
  region_readable_ = flash_.begin() &&
                     product_region::takeCensus(flash_, page_count_, census_);
  if (!region_readable_) {
    decision_ = product_region::MountDecision::kNoOwner;
    return;
  }
  decision_ = product_region::mountDecision(owner_, census_);
  // Only ObservationStore is composed here; a Gateway's CustodyStore owner
  // arrives with the Gateway service (SF5G).
  if (decision_ != product_region::MountDecision::kMount ||
      owner_ != product_region::Owner::kObservation)
    return;
  mounted_ = store_.begin(device_id);
}

ProductRecordStore::Submit ProductRecordStore::submitPeriodic(
    const PeriodicRecordInputs& inputs) {
  if (!mounted_ || !store_.ready() || store_.faulted() || preparing())
    return Submit::kNotMounted;
  if (pending_) {
    // The previous period is still being written. Periods are minutes
    // apart and a write takes milliseconds, so this means the store is
    // stuck; keep the older record rather than reorder identities.
    ++diagnostics_.dropped_busy;
    return Submit::kBusy;
  }
  pending_inputs_ = inputs;
  pending_identity_ = osf::RecordIdentity();
  pending_ = true;
  maintenance_rounds_ = 0;
  ++diagnostics_.submitted;
  return Submit::kAccepted;
}

bool ProductRecordStore::takeOutcome(Outcome& outcome) {
  if (!outcome_ready_) return false;
  outcome = outcome_;
  outcome_ready_ = false;
  return true;
}

void ProductRecordStore::finishPending(bool stored,
                                       const osf::RecordIdentity& identity) {
  outcome_.incarnation = identity.incarnation;
  outcome_.sequence = identity.sequence;
  outcome_.stored = stored;
  outcome_ready_ = true;
  if (stored)
    ++diagnostics_.stored;
  else
    ++diagnostics_.failed;
  pending_ = false;
  job_ = Job::kNone;
}


ObservationStore::LookupResult ProductRecordStore::oldestRetained(
    ObservationStore::Record& record) const {
  if (!mounted_ || preparing()) return ObservationStore::LookupResult::kReadError;
  return store_.oldestRetained(record);
}

ObservationStore::ControlLookupResult ProductRecordStore::findCachedExact(
    const osf::RecordIdentity& identity, osc::ExactObject& value) const {
  if (!mounted_ || preparing())
    return ObservationStore::ControlLookupResult::kReadError;
  return store_.findExactObject(identity, value);
}

bool ProductRecordStore::custodyCandidateMatchesRetainedExact(
    const osc::ExactObject& expected) const {
  if (!mounted_ || preparing() || pending_ ||
      job_ != Job::kNone || cache_pending_ || cache_outcome_ready_ ||
      !store_.ready() || store_.faulted() || store_.busy() ||
      flash_.hasUnreconciledMutation() ||
      !expected.identity.valid() ||
      (expected.record_kind != osf::RecordKind::kPeriodic &&
       expected.record_kind != osf::RecordKind::kEvent) ||
      expected.object_size == 0U ||
      expected.object_size > osc::kMaxProtectedObjectSize)
    return false;

  ObservationStore::Record backing{};
  if (store_.lookup(expected.identity, backing) !=
          ObservationStore::LookupResult::kFound ||
      backing.released || backing.release_uncertain ||
      backing.release_marker_exhausted ||
      backing.kind != expected.record_kind)
    return false;

  // The storage owner is opaque to ACK authority. This exact-byte equality
  // is necessary, but NOT sufficient, for authenticated custody release.
  return matchesCachedObject(expected);
}

bool ProductRecordStore::snapshotCustodyCandidates(
    osc::ExactObject (&objects)[osf::kMaxExactCustodyObjects],
    uint16_t& count) const {
  if (!mounted_ || preparing() || pending_ || outcome_ready_ ||
      job_ != Job::kNone || cache_pending_ || cache_outcome_ready_ ||
      !store_.ready() || store_.faulted() || store_.busy() ||
      flash_.hasUnreconciledMutation())
    return false;
  return store_.snapshotRetainedExact(objects, count);
}

bool ProductRecordStore::matchesCachedObject(
    const osc::ExactObject& expected) const {
  osc::ExactObject found{};
  if (store_.findExactObject(expected.identity, found) !=
      ObservationStore::ControlLookupResult::kFound)
    return false;
  return found.identity.incarnation == expected.identity.incarnation &&
         found.identity.sequence == expected.identity.sequence &&
         found.record_kind == expected.record_kind &&
         found.object_size == expected.object_size &&
         memcmp(found.object, expected.object, expected.object_size) == 0;
}

ProductRecordStore::CacheSubmit ProductRecordStore::startCache(
    bool allow_control_maintenance) {
  switch (store_.requestPutExactObject(cache_object_)) {
    case ObservationStore::ControlWriteResult::kStarted:
      job_ = Job::kExactCache;
      return CacheSubmit::kStarted;
    case ObservationStore::ControlWriteResult::kAlreadySatisfied:
      return matchesCachedObject(cache_object_)
                 ? CacheSubmit::kAlreadyDurable : CacheSubmit::kRejected;
    case ObservationStore::ControlWriteResult::kBusy:
      return CacheSubmit::kBusy;
    case ObservationStore::ControlWriteResult::kRejected:
      return CacheSubmit::kRejected;
    case ObservationStore::ControlWriteResult::kNoCapacity: {
      if (!allow_control_maintenance) return CacheSubmit::kNoCapacity;
      uint16_t active = 0U;
      if (!store_.exactObjectCount(active)) return CacheSubmit::kRejected;
      if (active >= osf::kMaxExactCustodyObjects)
        return CacheSubmit::kNoCapacity;
      // First use and control compaction both require maintenance. One
      // attempt only; compaction cannot help a saturated four-object cache.
      switch (store_.requestControlMaintenance()) {
        case ObservationStore::MaintenanceResult::kStarted:
          job_ = Job::kCacheMaintenance;
          return CacheSubmit::kStarted;
        case ObservationStore::MaintenanceResult::kBusy:
          return CacheSubmit::kBusy;
        case ObservationStore::MaintenanceResult::kNoWork:
          return CacheSubmit::kNoCapacity;
        case ObservationStore::MaintenanceResult::kControlMaintenanceRequired:
        case ObservationStore::MaintenanceResult::kRejected:
          return CacheSubmit::kRejected;
      }
    }
  }
  return CacheSubmit::kRejected;
}

ProductRecordStore::CacheSubmit ProductRecordStore::submitExactCache(
    const osc::ExactObject& value) {
  if (!mounted_ || !store_.ready() || store_.faulted() || preparing())
    return CacheSubmit::kNotMounted;
  if (pending_ || job_ != Job::kNone || store_.busy() ||
      outcome_ready_ || cache_pending_ || cache_outcome_ready_)
    return CacheSubmit::kBusy;

  if (!value.identity.valid() ||
      (value.record_kind != osf::RecordKind::kPeriodic &&
       value.record_kind != osf::RecordKind::kEvent))
    return CacheSubmit::kRejected;
  tlp::ProductSecureFrame frame{};
  if (tlp::decodeProductSecureFrame(value.object, value.object_size, &frame) !=
          tlp::ProductSecureCodecResult::kOk ||
      frame.device_id != device_id_ ||
      (frame.path_flags & tlp::kProductPathFlagCustodyRequested) == 0U ||
      (value.record_kind == osf::RecordKind::kPeriodic &&
       frame.app_family != tlp::kProductAppFamilyPeriodic) ||
      (value.record_kind == osf::RecordKind::kEvent &&
       frame.app_family != tlp::kProductAppFamilyEvent))
    return CacheSubmit::kRejected;

  cache_object_ = value;
  const CacheSubmit result = startCache(true);
  if (result == CacheSubmit::kStarted) {
    cache_pending_ = true;
  } else {
    cache_object_ = osc::ExactObject{};
  }
  return result;
}

void ProductRecordStore::finishCache(bool durable) {
  cache_outcome_.identity = cache_object_.identity;
  cache_outcome_.durable = durable;
  cache_outcome_ready_ = true;
  cache_pending_ = false;
  cache_object_ = osc::ExactObject{};
  job_ = Job::kNone;
  if (durable)
    ++diagnostics_.exact_cache_committed;
  else
    ++diagnostics_.exact_cache_failed;
}

bool ProductRecordStore::takeCacheOutcome(CacheOutcome& outcome) {
  if (!cache_outcome_ready_) return false;
  outcome = cache_outcome_;
  cache_outcome_ready_ = false;
  return true;
}

void ProductRecordStore::poll() {
  if (preparing()) {
    drivePrepare();
    return;
  }
  if (!mounted_) return;
  store_.poll();
  if (store_.busy()) return;

  bool success = false;
  switch (job_) {
    case Job::kNone:
      break;
    case Job::kAppend: {
      ObservationStore::Handle handle;
      const bool taken = store_.takeAppendResult(success, handle);
      finishPending(taken && success,
                    taken ? handle.identity : pending_identity_);
      return;
    }
    case Job::kMaintenance:
      job_ = Job::kNone;
      ++diagnostics_.maintenance_runs;
      if (!store_.takeMaintenanceResult(success) || !success) {
        finishPending(false, pending_identity_);
        return;
      }
      break;
    case Job::kControl:
      job_ = Job::kNone;
      ++diagnostics_.maintenance_runs;
      if (!store_.takeControlMaintenanceResult(success) || !success) {
        finishPending(false, pending_identity_);
        return;
      }
      break;
    case Job::kExactCache:
      if (!store_.takeControlWriteResult(success)) {
        finishCache(false);
        return;
      }
      // The radio sender may not use kStarted or TX_DONE as custody proof.
      // Require readback of identical committed bytes before eligibility.
      finishCache(success && matchesCachedObject(cache_object_));
      return;
    case Job::kCacheMaintenance: {
      const bool completed = store_.takeControlMaintenanceResult(success);
      if (!completed || !success) {
        finishCache(false);
        return;
      }
      const CacheSubmit again = startCache(false);
      if (again == CacheSubmit::kStarted) return;
      finishCache(again == CacheSubmit::kAlreadyDurable);
      return;
    }
  }
  if (pending_) startNext();
}

void ProductRecordStore::startNext() {
  if (store_.faulted()) {
    finishPending(false, pending_identity_);
    return;
  }
  osf::RecordIdentity identity;
  if (!store_.peekNextIdentity(identity)) {
    // A rotation or compaction still resuming after a reset.
    runMaintenance();
    return;
  }
  PeriodicRecordInputs inputs = pending_inputs_;
  inputs.record_incarnation = identity.incarnation;
  inputs.record_sequence = identity.sequence;
  tlp::PeriodicObservationPlaintext record{};
  uint8_t bytes[tlp::kPeriodicPlaintextSize];
  if (buildPeriodicPlaintext(inputs, &record) !=
          tlp::ProductSecureCodecResult::kOk ||
      tlp::encodePeriodicPlaintext(record, bytes, sizeof(bytes)) !=
          tlp::ProductSecureCodecResult::kOk) {
    finishPending(false, identity);
    return;
  }
  pending_identity_ = identity;
  switch (store_.requestAppend(osf::RecordKind::kPeriodic,
                               osf::kProductSchemaV1, bytes, sizeof(bytes))) {
    case ObservationStore::AppendResult::kStarted:
      job_ = Job::kAppend;
      return;
    case ObservationStore::AppendResult::kBusy:
      return;  // retried on a later poll
    case ObservationStore::AppendResult::kNoCapacity:
    case ObservationStore::AppendResult::kRejected:
      runMaintenance();
      return;
  }
}

void ProductRecordStore::runMaintenance() {
  if (++maintenance_rounds_ > kMaxMaintenanceRounds) {
    finishPending(false, pending_identity_);
    return;
  }
  const ObservationStore::MaintenanceResult data = store_.requestMaintenance();
  if (data == ObservationStore::MaintenanceResult::kStarted) {
    job_ = Job::kMaintenance;
    return;
  }
  if (data == ObservationStore::MaintenanceResult::kBusy) {
    --maintenance_rounds_;
    return;
  }
  const bool resume_control =
      data == ObservationStore::MaintenanceResult::kControlMaintenanceRequired ||
      (data == ObservationStore::MaintenanceResult::kRejected &&
       store_.ready() && !store_.faulted());
  if (resume_control && store_.requestControlMaintenance() ==
                            ObservationStore::MaintenanceResult::kStarted) {
    job_ = Job::kControl;
    return;
  }
  // No work although the append found no room, or maintenance refused:
  // never loop on it.
  finishPending(false, pending_identity_);
}

bool ProductRecordStore::requestPrepare() {
  // Keep destructive service work out of every owner job and unconsumed
  // cache outcome, including failures that have faulted the mounted store.
  if (preparing() || pending_ || cache_pending_ || cache_outcome_ready_ ||
      job_ != Job::kNone || store_.busy() || !region_readable_)
    return false;
  const bool leftovers =
      decision_ == product_region::MountDecision::kNeedsPreparation;
  const bool faulted_store = mounted_ && store_.faulted();
  if (!leftovers && !faulted_store) return false;
  if (flash_.hasUnreconciledMutation()) return false;
  mounted_ = false;
  prepare_page_ = 0;
  prepare_state_ = PrepareState::kErasing;
  return true;
}

void ProductRecordStore::drivePrepare() {
  FlashOpResult result;
  if (prepare_state_ == PrepareState::kWaiting) {
    result = flash_.pollPending();
    if (result == FlashOpResult::kPending) return;
  } else {
    result = flash_.erasePage(prepare_page_);
    if (result == FlashOpResult::kPending) {
      prepare_state_ = PrepareState::kWaiting;
      return;
    }
  }
  if (result != FlashOpResult::kDone || flash_.hasUnreconciledMutation()) {
    endPrepare(false);
    return;
  }
  prepare_state_ = PrepareState::kErasing;
  if (++prepare_page_ < page_count_) return;

  product_region::Census census{0U, 0U, 0U, 0U};
  endPrepare(product_region::takeCensus(flash_, page_count_, census) &&
             census.erased == page_count_);
}

void ProductRecordStore::endPrepare(bool success) {
  prepare_state_ = PrepareState::kIdle;
  prepare_success_ = success;
  prepare_result_ready_ = true;
}

bool ProductRecordStore::takePrepareResult(bool& success) {
  if (!prepare_result_ready_) return false;
  success = prepare_success_;
  prepare_result_ready_ = false;
  return true;
}

}  // namespace orun_tlp
