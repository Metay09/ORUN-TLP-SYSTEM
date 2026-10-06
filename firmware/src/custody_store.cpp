#include "custody_store.h"

#include <string.h>

namespace orun_tlp {
namespace csf = custody_store_format;

bool CustodyStore::readPage(uint16_t page,
                            csf::PageInspection& inspection) const {
  if (page >= page_count_) return false;
  uint8_t bytes[csf::kPageHeaderSize];
  if (!flash_.read(pageOffset(page), bytes, sizeof(bytes))) return false;
  return csf::inspectPageHeader(bytes, sizeof(bytes), inspection);
}

bool CustodyStore::readReclaimIntent(
    uint16_t page, csf::ReclaimInspection& inspection) const {
  if (page >= page_count_) return false;
  uint8_t bytes[csf::kPageHeaderSize];
  if (!flash_.read(pageOffset(page), bytes, sizeof(bytes))) return false;
  return csf::inspectReclaimIntent(bytes, sizeof(bytes), inspection);
}

bool CustodyStore::readRecord(uint16_t page, uint16_t slot,
                              csf::RecordInspection& inspection) const {
  if (page >= page_count_ || slot >= csf::kRecordsPerPage) return false;
  uint8_t bytes[csf::kRecordSize];
  if (!flash_.read(recordOffset(page, slot), bytes, sizeof(bytes))) return false;
  return csf::inspectRecord(bytes, sizeof(bytes), inspection);
}

bool CustodyStore::pageRecordAreaErased(uint16_t page) const {
  uint8_t chunk[64];
  uint32_t offset = pageOffset(page) + csf::kPageHeaderSize;
  const uint32_t end = pageOffset(page) + csf::kPageSize;
  while (offset < end) {
    const size_t size = (end - offset) < sizeof(chunk)
                            ? static_cast<size_t>(end - offset)
                            : sizeof(chunk);
    if (!flash_.read(offset, chunk, size) || !csf::erased(chunk, size))
      return false;
    offset += static_cast<uint32_t>(size);
  }
  return true;
}

bool CustodyStore::pageAllErased(uint16_t page) const {
  uint8_t chunk[64];
  uint32_t offset = pageOffset(page);
  const uint32_t end = offset + csf::kPageSize;
  while (offset < end) {
    const size_t size = (end - offset) < sizeof(chunk)
                            ? static_cast<size_t>(end - offset)
                            : sizeof(chunk);
    if (!flash_.read(offset, chunk, size) || !csf::erased(chunk, size))
      return false;
    offset += static_cast<uint32_t>(size);
  }
  return true;
}

bool CustodyStore::pageGenerationMatches(uint16_t page,
                                         uint64_t generation) const {
  csf::PageInspection inspection;
  if (!readPage(page, inspection)) return false;
  return inspection.evidence == csf::PageEvidence::kActive &&
         inspection.generation == generation;
}

bool CustodyStore::generationUnique(uint16_t page, uint64_t generation) const {
  for (uint16_t other = 0; other < page_count_; ++other) {
    if (other == page) continue;
    csf::PageInspection inspection;
    if (!readPage(other, inspection)) return false;
    if ((inspection.evidence == csf::PageEvidence::kActive ||
         inspection.evidence == csf::PageEvidence::kPrepared) &&
        inspection.generation == generation)
      return false;
  }
  return true;
}

bool CustodyStore::recover() {
  active_page_ = -1;
  prepared_page_ = -1;
  max_generation_ = 0;
  faulted_ = false;
  reclaim_intent_valid_ = false;
  reclaim_intent_blocked_ = false;
  reclaim_target_page_ = UINT16_MAX;
  reclaim_target_generation_ = 0;
  diagnostics_.recovered_held = 0;
  diagnostics_.recovered_handed_off = 0;
  diagnostics_.staged_records = 0;
  diagnostics_.partial_record_commits = 0;
  diagnostics_.uncertain_retire_markers = 0;

  uint64_t active_generation = 0;
  uint64_t prepared_generation = 0;

  // Pass 1: identify the authoritative append page and the single prepared
  // reserve. Do not yet call arbitrary damaged pages fatal: a committed
  // reclaim intent on the active page can prove that one such page was safe to
  // erase when power failed.
  for (uint16_t page = 0; page < page_count_; ++page) {
    csf::PageInspection header;
    if (!readPage(page, header)) return false;
    if (header.evidence != csf::PageEvidence::kActive &&
        header.evidence != csf::PageEvidence::kPrepared)
      continue;

    if (!generationUnique(page, header.generation)) {
      faulted_ = true;
      ++diagnostics_.recovery_faults;
      continue;
    }
    if (header.generation > max_generation_) max_generation_ = header.generation;

    if (header.evidence == csf::PageEvidence::kPrepared) {
      if (!pageRecordAreaErased(page) || prepared_page_ >= 0) {
        faulted_ = true;
        ++diagnostics_.recovery_faults;
        continue;
      }
      prepared_page_ = page;
      prepared_generation = header.generation;
    } else if (header.generation > active_generation) {
      active_generation = header.generation;
      active_page_ = page;
    }
  }

  if (prepared_page_ >= 0 && active_page_ >= 0 &&
      prepared_generation <= active_generation) {
    faulted_ = true;
    ++diagnostics_.recovery_faults;
  }

  // The current active page may carry one durable reclaim authorization for a
  // predecessor. That authorization lives outside the page being erased, so a
  // power cut during erase cannot destroy the proof that the predecessor was
  // already fully handed off.
  if (active_page_ >= 0) {
    csf::ReclaimInspection reclaim;
    if (!readReclaimIntent(static_cast<uint16_t>(active_page_), reclaim))
      return false;
    switch (reclaim.evidence) {
      case csf::ReclaimEvidence::kNone:
        break;
      case csf::ReclaimEvidence::kCommitted:
        if (reclaim.target_page >= page_count_ ||
            int(reclaim.target_page) == active_page_ ||
            reclaim.target_generation == 0) {
          faulted_ = true;
          ++diagnostics_.recovery_faults;
          ++diagnostics_.reclaim_intent_faults;
        } else {
          reclaim_intent_valid_ = true;
          reclaim_target_page_ = reclaim.target_page;
          reclaim_target_generation_ = reclaim.target_generation;
          ++diagnostics_.reclaim_intent_recoveries;
        }
        break;
      case csf::ReclaimEvidence::kStaged:
      case csf::ReclaimEvidence::kPartialCommit:
        // No exact committed intent means no erase authority. The intent slot
        // is no longer reusable, so this active page can continue accepting
        // records but cannot safely authorize a future reclaim.
        reclaim_intent_blocked_ = true;
        ++diagnostics_.reclaim_intent_faults;
        break;
      case csf::ReclaimEvidence::kCorrupt:
        // A commit-word-authoritative but corrupt intent could have preceded an
        // erase. The target can no longer be identified safely: fail closed.
        faulted_ = true;
        ++diagnostics_.recovery_faults;
        ++diagnostics_.reclaim_intent_faults;
        break;
    }
  }

  // Pass 2: classify records. A page named by the durable reclaim intent may
  // be partially erased/corrupt because the power cut happened during the
  // authorized erase; that page no longer owns custody. Unsupported-newer
  // format evidence is never auto-erased, even when an old intent points at
  // the page, to preserve downgrade/forward-compatibility safety.
  for (uint16_t page = 0; page < page_count_; ++page) {
    csf::PageInspection header;
    if (!readPage(page, header)) return false;
    const bool authorized_target =
        reclaim_intent_valid_ && page == reclaim_target_page_;

    if (header.evidence == csf::PageEvidence::kErased) {
      if (!authorized_target && !pageAllErased(page)) {
        faulted_ = true;
        ++diagnostics_.recovery_faults;
      }
      continue;
    }

    if (header.evidence == csf::PageEvidence::kUnsupported) {
      faulted_ = true;
      ++diagnostics_.recovery_faults;
      continue;
    }

    if (header.evidence == csf::PageEvidence::kCorrupt ||
        header.evidence == csf::PageEvidence::kStaged ||
        header.evidence == csf::PageEvidence::kPartialCommit ||
        header.evidence == csf::PageEvidence::kPartialActivation) {
      if (authorized_target) continue;
      if ((header.evidence == csf::PageEvidence::kStaged ||
           header.evidence == csf::PageEvidence::kStaged ||
           header.evidence == csf::PageEvidence::kPartialCommit ||
           header.evidence == csf::PageEvidence::kPartialActivation) &&
          pageRecordAreaErased(page)) {
        // A cut while preparing/activating an empty page cannot hide custody.
        // It merely consumes capacity until background maintenance erases it.
        continue;
      }
      faulted_ = true;
      ++diagnostics_.recovery_faults;
      continue;
    }

    if (header.evidence == csf::PageEvidence::kPrepared) {
      if (!pageRecordAreaErased(page)) {
        faulted_ = true;
        ++diagnostics_.recovery_faults;
      }
      continue;
    }

    if (authorized_target) {
      // If the old page still has a coherent header, it must still be the exact
      // generation named by the intent. A different valid generation means the
      // intent is stale against a reused page and must never authorize erase.
      if (header.generation != reclaim_target_generation_) {
        faulted_ = true;
        ++diagnostics_.recovery_faults;
      }
      continue;
    }

    for (uint16_t slot = 0; slot < csf::kRecordsPerPage; ++slot) {
      csf::RecordInspection record;
      if (!readRecord(page, slot, record)) return false;
      switch (record.evidence) {
        case csf::RecordEvidence::kErased:
          break;
        case csf::RecordEvidence::kHeld:
          ++diagnostics_.recovered_held;
          if (record.retire_uncertain)
            ++diagnostics_.uncertain_retire_markers;
          break;
        case csf::RecordEvidence::kHandedOff:
          ++diagnostics_.recovered_handed_off;
          break;
        case csf::RecordEvidence::kStaged:
          ++diagnostics_.staged_records;
          break;
        case csf::RecordEvidence::kPartialCommit:
          ++diagnostics_.partial_record_commits;
          break;
        case csf::RecordEvidence::kCorrupt:
          // commit==0 but CRC/static bytes do not verify. Such a record may
          // already have been ACKed, so continuing normal custody would hide a
          // possible data-loss event. Fail closed globally.
          faulted_ = true;
          ++diagnostics_.recovery_faults;
          break;
      }
    }
  }

  return true;
}

bool CustodyStore::begin() {
  ready_ = false;
  job_ = Job::kNone;
  phase_ = Phase::kNone;
  flash_op_awaiting_completion_ = false;
  diagnostics_ = {};
  custody_result_ready_ = false;
  handoff_result_ready_ = false;
  maintenance_result_ready_ = false;

  if (page_count_ < 2U) return false;
  if (!flash_.begin()) return false;
  if (!recover()) return false;
  ready_ = true;
  return true;
}

bool CustodyStore::findDuplicate(const uint8_t* object, size_t object_size,
                                 Handle& handle, bool& handed_off) const {
  bool found_handed_off = false;
  Handle handed_handle;
  for (uint16_t page = 0; page < page_count_; ++page) {
    if (reclaim_intent_valid_ && page == reclaim_target_page_) continue;
    csf::PageInspection header;
    if (!readPage(page, header)) return false;
    if (header.evidence != csf::PageEvidence::kActive) continue;
    for (uint16_t slot = 0; slot < csf::kRecordsPerPage; ++slot) {
      csf::RecordInspection record;
      if (!readRecord(page, slot, record)) return false;
      if (!csf::exactObject(record, object, object_size)) continue;
      const Handle candidate{page, slot, header.generation};
      if (record.evidence == csf::RecordEvidence::kHeld) {
        handle = candidate;
        handed_off = false;
        return true;
      }
      if (!found_handed_off) {
        found_handed_off = true;
        handed_handle = candidate;
      }
    }
  }
  if (found_handed_off) {
    handle = handed_handle;
    handed_off = true;
    return true;
  }
  return false;
}

bool CustodyStore::findAppendSlot(uint16_t& page, uint16_t& slot,
                                  bool& needs_activation) const {
  needs_activation = false;
  if (active_page_ >= 0) {
    for (uint16_t candidate = 0; candidate < csf::kRecordsPerPage;
         ++candidate) {
      csf::RecordInspection record;
      if (!readRecord(static_cast<uint16_t>(active_page_), candidate, record))
        return false;
      if (record.evidence == csf::RecordEvidence::kErased) {
        page = static_cast<uint16_t>(active_page_);
        slot = candidate;
        return true;
      }
    }
  }

  if (prepared_page_ >= 0) {
    page = static_cast<uint16_t>(prepared_page_);
    slot = 0;
    needs_activation = true;
    return true;
  }
  return false;
}

CustodyStore::AdmissionResult CustodyStore::requestCustody(
    const uint8_t* object, size_t object_size, Handle* duplicate_handle) {
  if (!ready_ || faulted_ || object == nullptr || object_size != csf::kObjectSize)
    return AdmissionResult::kRejected;
  if (busy() || custody_result_ready_ || handoff_result_ready_ ||
      maintenance_result_ready_)
    return AdmissionResult::kBusy;

  Handle duplicate;
  bool handed_off = false;
  if (findDuplicate(object, object_size, duplicate, handed_off)) {
    if (duplicate_handle != nullptr) *duplicate_handle = duplicate;
    if (handed_off) {
      ++diagnostics_.duplicate_handed_off;
      return AdmissionResult::kDuplicateHandedOff;
    }
    ++diagnostics_.duplicate_held;
    return AdmissionResult::kDuplicateHeld;
  }

  uint16_t page = 0;
  uint16_t slot = 0;
  bool needs_activation = false;
  if (!findAppendSlot(page, slot, needs_activation)) {
    ++diagnostics_.admission_no_capacity;
    return AdmissionResult::kNoCapacity;
  }

  csf::PageInspection header;
  if (!readPage(page, header)) return AdmissionResult::kRejected;
  if (needs_activation) {
    if (header.evidence != csf::PageEvidence::kPrepared)
      return AdmissionResult::kRejected;
  } else if (header.evidence != csf::PageEvidence::kActive) {
    return AdmissionResult::kRejected;
  }

  target_page_ = page;
  target_slot_ = slot;
  target_generation_ = header.generation;
  pending_handle_ = Handle{page, slot, header.generation};
  memcpy(pending_object_, object, csf::kObjectSize);
  csf::encodeRecord(object, object_size, record_blob_);
  job_ = Job::kAdmission;
  phase_ = needs_activation ? Phase::kActivatePage : Phase::kRecordBody;
  flash_op_awaiting_completion_ = false;
  ++diagnostics_.admissions_started;
  return AdmissionResult::kStarted;
}

bool CustodyStore::takeCustodyResult(bool& success, Handle& handle) {
  if (!custody_result_ready_) return false;
  success = custody_result_success_;
  handle = custody_result_handle_;
  custody_result_ready_ = false;
  return true;
}

bool CustodyStore::requestMarkEdgeDurableAccepted(
    const Handle& handle, const uint8_t* object, size_t object_size) {
  if (!ready_ || faulted_ || object == nullptr || object_size != csf::kObjectSize ||
      handle.page >= page_count_ || handle.slot >= csf::kRecordsPerPage ||
      handle.page_generation == 0)
    return false;
  if (busy() || custody_result_ready_ || handoff_result_ready_ ||
      maintenance_result_ready_)
    return false;
  if (!pageGenerationMatches(handle.page, handle.page_generation)) return false;

  csf::RecordInspection record;
  if (!readRecord(handle.page, handle.slot, record) ||
      !csf::exactObject(record, object, object_size))
    return false;

  if (record.evidence == csf::RecordEvidence::kHandedOff) {
    handoff_result_ready_ = true;
    handoff_result_success_ = true;
    return true;
  }
  if (record.evidence != csf::RecordEvidence::kHeld) return false;

  pending_handle_ = handle;
  memcpy(pending_object_, object, csf::kObjectSize);
  target_page_ = handle.page;
  target_slot_ = handle.slot;
  target_generation_ = handle.page_generation;
  job_ = Job::kHandoff;
  phase_ = Phase::kRetireRecord;
  flash_op_awaiting_completion_ = false;
  return true;
}

bool CustodyStore::takeHandoffResult(bool& success) {
  if (!handoff_result_ready_) return false;
  success = handoff_result_success_;
  handoff_result_ready_ = false;
  return true;
}

bool CustodyStore::pageReclaimable(uint16_t page) const {
  if (page >= page_count_ || int(page) == active_page_ ||
      int(page) == prepared_page_)
    return false;
  csf::PageInspection header;
  if (!readPage(page, header) || header.evidence != csf::PageEvidence::kActive)
    return false;

  for (uint16_t slot = 0; slot < csf::kRecordsPerPage; ++slot) {
    csf::RecordInspection record;
    if (!readRecord(page, slot, record)) return false;
    if (record.evidence == csf::RecordEvidence::kHeld ||
        record.evidence == csf::RecordEvidence::kCorrupt)
      return false;
  }
  return true;
}

int CustodyStore::findErasedPage() const {
  for (uint16_t page = 0; page < page_count_; ++page) {
    csf::PageInspection inspection;
    if (!readPage(page, inspection)) return -1;
    if (inspection.evidence == csf::PageEvidence::kErased &&
        pageAllErased(page))
      return page;
  }
  return -1;
}

int CustodyStore::findRepairableBlankPage() const {
  for (uint16_t page = 0; page < page_count_; ++page) {
    if (reclaim_intent_valid_ && page == reclaim_target_page_) continue;
    csf::PageInspection inspection;
    if (!readPage(page, inspection)) return -1;
    const bool interrupted_header =
        inspection.evidence == csf::PageEvidence::kStaged ||
        inspection.evidence == csf::PageEvidence::kPartialCommit ||
        inspection.evidence == csf::PageEvidence::kPartialActivation;
    if (interrupted_header && pageRecordAreaErased(page)) return page;
  }
  return -1;
}

int CustodyStore::findReclaimablePage() const {
  int selected = -1;
  uint64_t generation = UINT64_MAX;
  for (uint16_t page = 0; page < page_count_; ++page) {
    if (!pageReclaimable(page)) continue;
    csf::PageInspection header;
    if (!readPage(page, header)) return -1;
    if (header.generation < generation) {
      generation = header.generation;
      selected = page;
    }
  }
  return selected;
}

CustodyStore::MaintenanceResult CustodyStore::requestMaintenance() {
  if (!ready_ || faulted_) return MaintenanceResult::kRejected;
  if (busy() || custody_result_ready_ || handoff_result_ready_ ||
      maintenance_result_ready_)
    return MaintenanceResult::kBusy;
  if (prepared_page_ >= 0) return MaintenanceResult::kNoWork;
  if (max_generation_ == UINT64_MAX) return MaintenanceResult::kRejected;

  // Resume a previously committed reclaim transaction before doing anything
  // else. This is what makes a power cut during page erase recoverable without
  // guessing that a corrupt page was safe to destroy.
  if (reclaim_intent_valid_) {
    if (reclaim_target_page_ >= page_count_ ||
        int(reclaim_target_page_) == active_page_)
      return MaintenanceResult::kRejected;

    csf::PageInspection target;
    if (!readPage(reclaim_target_page_, target))
      return MaintenanceResult::kRejected;

    if ((target.evidence == csf::PageEvidence::kActive ||
         target.evidence == csf::PageEvidence::kPrepared) &&
        target.generation != reclaim_target_generation_) {
      // Never apply an old intent to a page that has already been reused.
      return MaintenanceResult::kRejected;
    }
    if (target.evidence == csf::PageEvidence::kUnsupported)
      return MaintenanceResult::kRejected;

    target_page_ = reclaim_target_page_;
    target_slot_ = UINT16_MAX;
    target_generation_ = max_generation_ + 1U;
    csf::encodePageHeader(target_generation_, page_blob_);
    job_ = Job::kMaintenance;
    phase_ = pageAllErased(target_page_) ? Phase::kHeaderBody
                                         : Phase::kErasePage;
    flash_op_awaiting_completion_ = false;
    return MaintenanceResult::kStarted;
  }

  int page = findErasedPage();
  if (page >= 0) {
    target_page_ = static_cast<uint16_t>(page);
    target_slot_ = UINT16_MAX;
    target_generation_ = max_generation_ + 1U;
    csf::encodePageHeader(target_generation_, page_blob_);
    job_ = Job::kMaintenance;
    phase_ = Phase::kHeaderBody;
    flash_op_awaiting_completion_ = false;
    return MaintenanceResult::kStarted;
  }

  page = findRepairableBlankPage();
  if (page >= 0) {
    target_page_ = static_cast<uint16_t>(page);
    target_slot_ = UINT16_MAX;
    target_generation_ = max_generation_ + 1U;
    csf::encodePageHeader(target_generation_, page_blob_);
    job_ = Job::kMaintenance;
    phase_ = Phase::kErasePage;
    flash_op_awaiting_completion_ = false;
    return MaintenanceResult::kStarted;
  }

  if (reclaim_intent_blocked_ || active_page_ < 0)
    return MaintenanceResult::kNoWork;

  page = findReclaimablePage();
  if (page < 0) return MaintenanceResult::kNoWork;

  csf::PageInspection old_header;
  if (!readPage(static_cast<uint16_t>(page), old_header) ||
      old_header.evidence != csf::PageEvidence::kActive)
    return MaintenanceResult::kRejected;

  target_page_ = static_cast<uint16_t>(page);
  target_slot_ = UINT16_MAX;
  reclaim_target_page_ = target_page_;
  reclaim_target_generation_ = old_header.generation;
  intent_owner_page_ = active_page_;
  csf::encodeReclaimIntent(reclaim_target_page_, reclaim_target_generation_,
                           intent_blob_);
  target_generation_ = max_generation_ + 1U;
  csf::encodePageHeader(target_generation_, page_blob_);
  job_ = Job::kMaintenance;
  phase_ = Phase::kIntentBody;
  flash_op_awaiting_completion_ = false;
  return MaintenanceResult::kStarted;
}

bool CustodyStore::takeMaintenanceResult(bool& success) {
  if (!maintenance_result_ready_) return false;
  success = maintenance_result_success_;
  maintenance_result_ready_ = false;
  return true;
}

FlashOpResult CustodyStore::programStep(uint32_t offset, const void* data,
                                        size_t size) {
  if (flash_op_awaiting_completion_) {
    const FlashOpResult result = flash_.pollPending();
    if (result != FlashOpResult::kPending) flash_op_awaiting_completion_ = false;
    return result;
  }
  const FlashOpResult result = flash_.program(offset, data, size);
  if (result == FlashOpResult::kPending) flash_op_awaiting_completion_ = true;
  return result;
}

FlashOpResult CustodyStore::eraseStep(uint16_t page) {
  if (flash_op_awaiting_completion_) {
    const FlashOpResult result = flash_.pollPending();
    if (result != FlashOpResult::kPending) flash_op_awaiting_completion_ = false;
    return result;
  }
  const FlashOpResult result = flash_.erasePage(page);
  if (result == FlashOpResult::kPending) flash_op_awaiting_completion_ = true;
  return result;
}

void CustodyStore::finishAdmission(bool success) {
  custody_result_ready_ = true;
  custody_result_success_ = success;
  custody_result_handle_ = pending_handle_;
  if (success)
    ++diagnostics_.admissions_committed;
  else
    ++diagnostics_.admission_failures;
  job_ = Job::kNone;
  phase_ = Phase::kNone;
  flash_op_awaiting_completion_ = false;
}

void CustodyStore::finishHandoff(bool success) {
  handoff_result_ready_ = true;
  handoff_result_success_ = success;
  if (success)
    ++diagnostics_.handoffs_committed;
  else
    ++diagnostics_.handoff_failures;
  job_ = Job::kNone;
  phase_ = Phase::kNone;
  flash_op_awaiting_completion_ = false;
}

void CustodyStore::finishMaintenance(bool success) {
  maintenance_result_ready_ = true;
  maintenance_result_success_ = success;
  if (!success) ++diagnostics_.maintenance_failures;
  job_ = Job::kNone;
  phase_ = Phase::kNone;
  flash_op_awaiting_completion_ = false;
}

void CustodyStore::failCurrentJob() {
  if (flash_.hasUnreconciledMutation()) {
    faulted_ = true;
    ++diagnostics_.unreconciled_mutation_faults;
  }
  switch (job_) {
    case Job::kAdmission:
      finishAdmission(false);
      break;
    case Job::kHandoff:
      finishHandoff(false);
      break;
    case Job::kMaintenance:
      finishMaintenance(false);
      break;
    case Job::kNone:
      break;
  }
}

void CustodyStore::poll() {
  if (!ready_ || job_ == Job::kNone) return;
  static const uint32_t kZero = 0U;

  if (job_ == Job::kAdmission) {
    if (phase_ == Phase::kActivatePage) {
      const FlashOpResult result =
          programStep(pageOffset(target_page_) + csf::kPageHeaderActiveOffset,
                      &kZero, sizeof(kZero));
      if (result == FlashOpResult::kPending) return;
      if (result == FlashOpResult::kFailed) return failCurrentJob();
      uint8_t verify[4];
      if (!flash_.read(pageOffset(target_page_) + csf::kPageHeaderActiveOffset,
                       verify, sizeof(verify)) || csf::get32(verify) != 0U)
        return failCurrentJob();
      active_page_ = target_page_;
      prepared_page_ = -1;
      // A newly activated prepared page has a pristine reclaim-intent slot.
      reclaim_intent_valid_ = false;
      reclaim_intent_blocked_ = false;
      reclaim_target_page_ = UINT16_MAX;
      reclaim_target_generation_ = 0;
      phase_ = Phase::kRecordBody;
      return;
    }

    if (phase_ == Phase::kRecordBody) {
      const FlashOpResult result =
          programStep(recordOffset(target_page_, target_slot_), record_blob_,
                      csf::kRecordCommitOffset);
      if (result == FlashOpResult::kPending) return;
      if (result == FlashOpResult::kFailed) return failCurrentJob();
      uint8_t verify[csf::kRecordCommitOffset];
      if (!flash_.read(recordOffset(target_page_, target_slot_), verify,
                       sizeof(verify)) ||
          memcmp(verify, record_blob_, sizeof(verify)) != 0)
        return failCurrentJob();
      phase_ = Phase::kRecordCommit;
      return;
    }

    if (phase_ == Phase::kRecordCommit) {
      const FlashOpResult result = programStep(
          recordOffset(target_page_, target_slot_) + csf::kRecordCommitOffset,
          record_blob_ + csf::kRecordCommitOffset, 4U);
      if (result == FlashOpResult::kPending) return;
      if (result == FlashOpResult::kFailed) return failCurrentJob();
      csf::RecordInspection record;
      if (!pageGenerationMatches(target_page_, target_generation_) ||
          !readRecord(target_page_, target_slot_, record) ||
          record.evidence != csf::RecordEvidence::kHeld ||
          !csf::exactObject(record, pending_object_, csf::kObjectSize))
        return failCurrentJob();
      finishAdmission(true);
      return;
    }
  }

  if (job_ == Job::kHandoff && phase_ == Phase::kRetireRecord) {
    const FlashOpResult result = programStep(
        recordOffset(target_page_, target_slot_) + csf::kRecordRetireOffset,
        &kZero, sizeof(kZero));
    if (result == FlashOpResult::kPending) return;
    if (result == FlashOpResult::kFailed) return failCurrentJob();
    csf::RecordInspection record;
    if (!pageGenerationMatches(target_page_, target_generation_) ||
        !readRecord(target_page_, target_slot_, record) ||
        record.evidence != csf::RecordEvidence::kHandedOff ||
        !csf::exactObject(record, pending_object_, csf::kObjectSize))
      return failCurrentJob();
    finishHandoff(true);
    return;
  }

  if (job_ == Job::kMaintenance) {
    if (phase_ == Phase::kIntentBody) {
      const uint32_t offset = pageOffset(static_cast<uint16_t>(intent_owner_page_)) +
                              csf::kReclaimIntentOffset;
      constexpr size_t kBodyAndCrc =
          csf::kReclaimIntentCrcOffset - csf::kReclaimIntentOffset + 4U;
      const FlashOpResult result =
          programStep(offset, intent_blob_, kBodyAndCrc);
      if (result == FlashOpResult::kPending) return;
      if (result == FlashOpResult::kFailed) return failCurrentJob();
      uint8_t verify[kBodyAndCrc];
      if (!flash_.read(offset, verify, sizeof(verify)) ||
          memcmp(verify, intent_blob_, sizeof(verify)) != 0)
        return failCurrentJob();
      phase_ = Phase::kIntentCommit;
      return;
    }

    if (phase_ == Phase::kIntentCommit) {
      const uint32_t offset = pageOffset(static_cast<uint16_t>(intent_owner_page_)) +
                              csf::kReclaimIntentCommitOffset;
      constexpr size_t kCommitInBlob =
          csf::kReclaimIntentCommitOffset - csf::kReclaimIntentOffset;
      const FlashOpResult result =
          programStep(offset, intent_blob_ + kCommitInBlob, 4U);
      if (result == FlashOpResult::kPending) return;
      if (result == FlashOpResult::kFailed) return failCurrentJob();
      csf::ReclaimInspection reclaim;
      if (!readReclaimIntent(static_cast<uint16_t>(intent_owner_page_), reclaim) ||
          reclaim.evidence != csf::ReclaimEvidence::kCommitted ||
          reclaim.target_page != reclaim_target_page_ ||
          reclaim.target_generation != reclaim_target_generation_)
        return failCurrentJob();
      reclaim_intent_valid_ = true;
      ++diagnostics_.reclaim_intents_committed;
      phase_ = Phase::kErasePage;
      return;
    }

    if (phase_ == Phase::kErasePage) {
      const FlashOpResult result = eraseStep(target_page_);
      if (result == FlashOpResult::kPending) return;
      if (result == FlashOpResult::kFailed) return failCurrentJob();
      if (!pageAllErased(target_page_)) return failCurrentJob();
      ++diagnostics_.pages_reclaimed;
      phase_ = Phase::kHeaderBody;
      return;
    }

    if (phase_ == Phase::kHeaderBody) {
      const FlashOpResult result =
          programStep(pageOffset(target_page_), page_blob_,
                      csf::kPageHeaderCommitOffset);
      if (result == FlashOpResult::kPending) return;
      if (result == FlashOpResult::kFailed) return failCurrentJob();
      uint8_t verify[csf::kPageHeaderCommitOffset];
      if (!flash_.read(pageOffset(target_page_), verify, sizeof(verify)) ||
          memcmp(verify, page_blob_, sizeof(verify)) != 0)
        return failCurrentJob();
      phase_ = Phase::kHeaderCommit;
      return;
    }

    if (phase_ == Phase::kHeaderCommit) {
      const FlashOpResult result = programStep(
          pageOffset(target_page_) + csf::kPageHeaderCommitOffset,
          page_blob_ + csf::kPageHeaderCommitOffset, 4U);
      if (result == FlashOpResult::kPending) return;
      if (result == FlashOpResult::kFailed) return failCurrentJob();
      csf::PageInspection header;
      if (!readPage(target_page_, header) ||
          header.evidence != csf::PageEvidence::kPrepared ||
          header.generation != target_generation_ ||
          !pageRecordAreaErased(target_page_))
        return failCurrentJob();
      prepared_page_ = target_page_;
      max_generation_ = target_generation_;
      ++diagnostics_.pages_prepared;
      finishMaintenance(true);
      return;
    }
  }

  failCurrentJob();
}

bool CustodyStore::oldestHeld(
    Handle& handle, uint8_t object[csf::kObjectSize]) const {
  bool found = false;
  uint64_t best_generation = UINT64_MAX;
  uint16_t best_slot = UINT16_MAX;
  Handle best;
  uint8_t best_object[csf::kObjectSize]{};

  for (uint16_t page = 0; page < page_count_; ++page) {
    if (reclaim_intent_valid_ && page == reclaim_target_page_) continue;
    csf::PageInspection header;
    if (!readPage(page, header)) return false;
    if (header.evidence != csf::PageEvidence::kActive) continue;
    for (uint16_t slot = 0; slot < csf::kRecordsPerPage; ++slot) {
      csf::RecordInspection record;
      if (!readRecord(page, slot, record)) return false;
      if (record.evidence != csf::RecordEvidence::kHeld) continue;
      if (!found || header.generation < best_generation ||
          (header.generation == best_generation && slot < best_slot)) {
        found = true;
        best_generation = header.generation;
        best_slot = slot;
        best = Handle{page, slot, header.generation};
        memcpy(best_object, record.object, csf::kObjectSize);
      }
    }
  }
  if (!found) return false;
  handle = best;
  memcpy(object, best_object, csf::kObjectSize);
  return true;
}

uint32_t CustodyStore::heldCount() const {
  uint32_t count = 0;
  for (uint16_t page = 0; page < page_count_; ++page) {
    if (reclaim_intent_valid_ && page == reclaim_target_page_) continue;
    csf::PageInspection header;
    if (!readPage(page, header) ||
        header.evidence != csf::PageEvidence::kActive)
      continue;
    for (uint16_t slot = 0; slot < csf::kRecordsPerPage; ++slot) {
      csf::RecordInspection record;
      if (readRecord(page, slot, record) &&
          record.evidence == csf::RecordEvidence::kHeld)
        ++count;
    }
  }
  return count;
}

uint32_t CustodyStore::handedOffCount() const {
  uint32_t count = 0;
  for (uint16_t page = 0; page < page_count_; ++page) {
    if (reclaim_intent_valid_ && page == reclaim_target_page_) continue;
    csf::PageInspection header;
    if (!readPage(page, header) ||
        header.evidence != csf::PageEvidence::kActive)
      continue;
    for (uint16_t slot = 0; slot < csf::kRecordsPerPage; ++slot) {
      csf::RecordInspection record;
      if (readRecord(page, slot, record) &&
          record.evidence == csf::RecordEvidence::kHandedOff)
        ++count;
    }
  }
  return count;
}

}  // namespace orun_tlp
