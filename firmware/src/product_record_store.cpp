#include "product_record_store.h"

namespace orun_tlp {

void ProductRecordStore::begin(uint64_t device_id,
                               const product_region::Jobs& jobs) {
  mounted_ = false;
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
  if (preparing() || pending_ || !region_readable_) return false;
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
