#include "observation_store.h"

#include <string.h>

namespace orun_tlp {
namespace {

bool sameIdentity(const osf::RecordIdentity& a,
                  const osf::RecordIdentity& b) {
  return a.incarnation == b.incarnation && a.sequence == b.sequence;
}

bool generationSeen(const uint64_t* generations, uint16_t count,
                    uint64_t generation) {
  for (uint16_t i = 0; i < count; ++i)
    if (generations[i] == generation) return true;
  return false;
}

}  // namespace

bool ObservationStore::readPage(
    uint16_t page, osf::PageInspection& inspection) const {
  if (page >= page_count_) return false;
  uint8_t bytes[osf::kPageHeaderSize];
  if (!flash_.read(pageOffset(page), bytes, sizeof(bytes))) return false;
  return osf::inspectPageHeader(bytes, sizeof(bytes), inspection);
}

bool ObservationStore::readRecord(
    uint16_t page, uint16_t slot, osf::RecordInspection& inspection) const {
  if (page < osf::kControlPageCount || page >= page_count_ ||
      slot >= osf::kDataRecordsPerPage)
    return false;
  uint8_t bytes[osf::kDataRecordSize];
  if (!flash_.read(recordOffset(page, slot), bytes, sizeof(bytes))) return false;
  return osf::inspectRecord(bytes, sizeof(bytes), inspection);
}

bool ObservationStore::pageAllErased(uint16_t page) const {
  if (page >= page_count_) return false;
  uint8_t chunk[64];
  uint32_t offset = pageOffset(page);
  const uint32_t end = offset + osf::kPageSize;
  while (offset < end) {
    const size_t size =
        (end - offset) < sizeof(chunk)
            ? static_cast<size_t>(end - offset)
            : sizeof(chunk);
    if (!flash_.read(offset, chunk, size) || !osf::erased(chunk, size))
      return false;
    offset += static_cast<uint32_t>(size);
  }
  return true;
}

bool ObservationStore::pagePayloadErased(uint16_t page) const {
  if (page >= page_count_) return false;
  uint8_t chunk[64];
  uint32_t offset = pageOffset(page) + osf::kPageHeaderSize;
  const uint32_t end = pageOffset(page) + osf::kPageSize;
  while (offset < end) {
    const size_t size =
        (end - offset) < sizeof(chunk)
            ? static_cast<size_t>(end - offset)
            : sizeof(chunk);
    if (!flash_.read(offset, chunk, size) || !osf::erased(chunk, size))
      return false;
    offset += static_cast<uint32_t>(size);
  }
  return true;
}

bool ObservationStore::pageGenerationMatches(
    uint16_t page, uint64_t generation) const {
  osf::PageInspection inspection;
  return readPage(page, inspection) &&
         inspection.evidence == osf::PageEvidence::kActive &&
         inspection.kind == osf::PageKind::kData &&
         inspection.generation == generation &&
         inspection.device_id == device_id_ &&
         inspection.incarnation == incarnation_;
}

bool ObservationStore::recover() {
  active_data_page_ = -1;
  prepared_data_page_ = -1;
  max_data_generation_ = 0;
  next_sequence_ = 1U;

  diagnostics_.recovered_retained = 0;
  diagnostics_.recovered_released = 0;
  diagnostics_.staged_records = 0;
  diagnostics_.partial_record_commits = 0;

  uint64_t recovered_incarnation = 0;
  uint64_t generations[kMaxPages]{};
  uint16_t generation_count = 0;
  PageSummary summaries[kMaxPages]{};
  uint16_t summary_count = 0;
  uint32_t max_sequence = 0;
  uint64_t active_generation = 0;
  uint64_t prepared_generation = 0;

  for (uint16_t page = 0; page < page_count_; ++page) {
    osf::PageInspection header;
    if (!readPage(page, header)) return false;

    const bool control_region = page < osf::kControlPageCount;
    if (header.evidence == osf::PageEvidence::kErased) {
      if (!pageAllErased(page)) {
        faulted_ = true;
        ++diagnostics_.recovery_faults;
      }
      continue;
    }

    if (header.evidence == osf::PageEvidence::kStaged ||
        header.evidence == osf::PageEvidence::kPartialCommit ||
        header.evidence == osf::PageEvidence::kPartialActivation) {
      if (!pagePayloadErased(page)) {
        faulted_ = true;
        ++diagnostics_.recovery_faults;
      }
      continue;
    }

    if (header.evidence == osf::PageEvidence::kCorrupt ||
        header.evidence == osf::PageEvidence::kUnsupported) {
      faulted_ = true;
      ++diagnostics_.recovery_faults;
      continue;
    }

    if (header.device_id != device_id_ ||
        header.incarnation == 0U ||
        (control_region && header.kind != osf::PageKind::kControl) ||
        (!control_region && header.kind != osf::PageKind::kData)) {
      faulted_ = true;
      ++diagnostics_.recovery_faults;
      continue;
    }

    if (recovered_incarnation == 0U)
      recovered_incarnation = header.incarnation;
    else if (recovered_incarnation != header.incarnation) {
      faulted_ = true;
      ++diagnostics_.recovery_faults;
      continue;
    }

    if (control_region) {
      // SF5C core lands before the bounded control journal implementation.
      // A committed empty control page is accepted so the next slice can be
      // integrated without changing the data-page recovery contract. Any
      // committed control payload fails closed until that owner exists.
      for (uint16_t slot = 0; slot < osf::kControlRecordsPerPage; ++slot) {
        uint8_t bytes[osf::kControlRecordSize];
        const uint32_t offset =
            pageOffset(page) + osf::kPageHeaderSize +
            uint32_t(slot) * osf::kControlRecordSize;
        if (!flash_.read(offset, bytes, sizeof(bytes))) return false;
        if (!osf::erased(bytes, sizeof(bytes))) {
          faulted_ = true;
          ++diagnostics_.recovery_faults;
          break;
        }
      }
      continue;
    }

    if (generationSeen(generations, generation_count, header.generation)) {
      faulted_ = true;
      ++diagnostics_.recovery_faults;
      continue;
    }
    if (generation_count >= kMaxPages) return false;
    generations[generation_count++] = header.generation;
    if (header.generation > max_data_generation_)
      max_data_generation_ = header.generation;

    if (header.evidence == osf::PageEvidence::kPrepared) {
      if (!pagePayloadErased(page) || prepared_data_page_ >= 0) {
        faulted_ = true;
        ++diagnostics_.recovery_faults;
        continue;
      }
      prepared_data_page_ = page;
      prepared_generation = header.generation;
      continue;
    }

    if (header.evidence != osf::PageEvidence::kActive) {
      faulted_ = true;
      ++diagnostics_.recovery_faults;
      continue;
    }

    if (header.generation > active_generation) {
      active_generation = header.generation;
      active_data_page_ = page;
    }

    PageSummary summary;
    summary.page = page;
    summary.generation = header.generation;
    uint32_t previous_sequence = 0;

    for (uint16_t slot = 0; slot < osf::kDataRecordsPerPage; ++slot) {
      osf::RecordInspection record;
      if (!readRecord(page, slot, record)) return false;

      if (record.evidence == osf::RecordEvidence::kErased) continue;
      if (record.evidence == osf::RecordEvidence::kStaged) {
        ++diagnostics_.staged_records;
        continue;
      }
      if (record.evidence == osf::RecordEvidence::kPartialCommit) {
        ++diagnostics_.partial_record_commits;
        continue;
      }
      if (record.evidence == osf::RecordEvidence::kCorrupt ||
          record.incarnation != header.incarnation ||
          record.sequence == 0U ||
          (previous_sequence != 0U &&
           record.sequence <= previous_sequence)) {
        faulted_ = true;
        ++diagnostics_.recovery_faults;
        continue;
      }

      previous_sequence = record.sequence;
      if (summary.first_sequence == 0U)
        summary.first_sequence = record.sequence;
      summary.last_sequence = record.sequence;
      if (record.sequence > max_sequence) max_sequence = record.sequence;

      if (record.evidence == osf::RecordEvidence::kReleased)
        ++diagnostics_.recovered_released;
      else
        ++diagnostics_.recovered_retained;
    }

    if (summary_count >= kMaxPages) return false;
    summaries[summary_count++] = summary;
  }

  if (faulted_) return true;

  // Retained data-page generations must preserve strictly increasing sequence
  // ranges. This makes one tracker-wide sequence namespace recoverable without
  // a second persistent counter journal in the first SF5C slice.
  uint64_t previous_generation = 0;
  uint32_t previous_last_sequence = 0;
  for (uint16_t consumed = 0; consumed < summary_count; ++consumed) {
    int selected = -1;
    uint64_t selected_generation = UINT64_MAX;
    for (uint16_t i = 0; i < summary_count; ++i) {
      if (summaries[i].generation > previous_generation &&
          summaries[i].generation < selected_generation) {
        selected = static_cast<int>(i);
        selected_generation = summaries[i].generation;
      }
    }
    if (selected < 0) {
      faulted_ = true;
      ++diagnostics_.recovery_faults;
      break;
    }
    const PageSummary& current = summaries[selected];
    if (current.first_sequence != 0U && previous_last_sequence != 0U &&
        current.first_sequence <= previous_last_sequence) {
      faulted_ = true;
      ++diagnostics_.recovery_faults;
      break;
    }
    if (current.last_sequence != 0U)
      previous_last_sequence = current.last_sequence;
    previous_generation = current.generation;
  }

  if (faulted_) return true;

  if (prepared_data_page_ >= 0) {
    const uint64_t expected =
        active_generation == 0U ? 1U : active_generation + 1U;
    if (active_generation == UINT64_MAX ||
        prepared_generation != expected) {
      faulted_ = true;
      ++diagnostics_.recovery_faults;
      return true;
    }
  }

  incarnation_ = recovered_incarnation;
  if (max_sequence == UINT32_MAX)
    next_sequence_ = 0U;
  else
    next_sequence_ = max_sequence + 1U;
  return true;
}

bool ObservationStore::begin(uint64_t device_id) {
  ready_ = false;
  faulted_ = false;
  flash_op_awaiting_completion_ = false;
  job_ = Job::kNone;
  phase_ = Phase::kNone;
  diagnostics_ = Diagnostics();
  append_result_ready_ = false;
  release_result_ready_ = false;
  maintenance_result_ready_ = false;
  device_id_ = device_id;
  incarnation_ = 0;
  next_sequence_ = 1U;
  max_data_generation_ = 0;
  active_data_page_ = -1;
  prepared_data_page_ = -1;

  if (device_id_ == 0U || page_count_ < osf::kControlPageCount + 2U ||
      page_count_ > kMaxPages || incarnation_source_ == nullptr)
    return false;
  if (!flash_.begin()) return false;
  if (flash_.hasUnreconciledMutation()) {
    faulted_ = true;
    ++diagnostics_.unreconciled_mutation_faults;
    return false;
  }
  if (!recover()) return false;
  if (faulted_) return true;

  if (incarnation_ == 0U) {
    uint64_t fresh = 0;
    if (!incarnation_source_->generate(fresh) || fresh == 0U) return false;
    incarnation_ = fresh;
  }

  ready_ = true;
  return true;
}

bool ObservationStore::peekNextIdentity(osf::RecordIdentity& identity) const {
  identity = osf::RecordIdentity();
  if (!ready_ || faulted_ || busy() || next_sequence_ == 0U ||
      incarnation_ == 0U)
    return false;
  identity.incarnation = incarnation_;
  identity.sequence = next_sequence_;
  return true;
}

bool ObservationStore::findAppendSlot(
    uint16_t& page, uint16_t& slot, bool& needs_activation) const {
  needs_activation = false;

  if (active_data_page_ >= 0) {
    for (uint16_t candidate = 0; candidate < osf::kDataRecordsPerPage;
         ++candidate) {
      osf::RecordInspection record;
      if (!readRecord(static_cast<uint16_t>(active_data_page_), candidate,
                      record))
        return false;
      if (record.evidence == osf::RecordEvidence::kErased) {
        page = static_cast<uint16_t>(active_data_page_);
        slot = candidate;
        return true;
      }
    }
  }

  if (prepared_data_page_ >= 0) {
    osf::PageInspection header;
    if (!readPage(static_cast<uint16_t>(prepared_data_page_), header) ||
        header.evidence != osf::PageEvidence::kPrepared ||
        header.kind != osf::PageKind::kData ||
        header.device_id != device_id_ ||
        header.incarnation != incarnation_)
      return false;
    page = static_cast<uint16_t>(prepared_data_page_);
    slot = 0U;
    needs_activation = true;
    return true;
  }

  return false;
}

ObservationStore::AppendResult ObservationStore::requestAppend(
    osf::RecordKind kind, uint8_t schema,
    const uint8_t* payload, size_t payload_size) {
  if (!ready_ || faulted_ || next_sequence_ == 0U || schema == 0U ||
      payload_size > osf::kDataPayloadSize ||
      (payload_size != 0U && payload == nullptr))
    return AppendResult::kRejected;
  if (busy() || append_result_ready_ || release_result_ready_ ||
      maintenance_result_ready_)
    return AppendResult::kBusy;

  osf::RecordIdentity identity;
  if (!peekNextIdentity(identity)) return AppendResult::kRejected;

  uint16_t page = 0;
  uint16_t slot = 0;
  bool needs_activation = false;
  if (!findAppendSlot(page, slot, needs_activation))
    return AppendResult::kNoCapacity;

  osf::PageInspection header;
  if (!readPage(page, header)) return AppendResult::kRejected;
  if ((needs_activation &&
       header.evidence != osf::PageEvidence::kPrepared) ||
      (!needs_activation &&
       header.evidence != osf::PageEvidence::kActive) ||
      header.kind != osf::PageKind::kData ||
      header.device_id != device_id_ ||
      header.incarnation != incarnation_)
    return AppendResult::kRejected;

  if (!osf::encodeRecord(kind, schema, identity.incarnation,
                         identity.sequence, payload, payload_size,
                         record_blob_))
    return AppendResult::kRejected;

  target_page_ = page;
  target_slot_ = slot;
  target_generation_ = header.generation;
  pending_handle_.page = page;
  pending_handle_.slot = slot;
  pending_handle_.page_generation = header.generation;
  pending_handle_.identity = identity;
  pending_kind_ = kind;
  pending_schema_ = schema;
  pending_payload_size_ = static_cast<uint8_t>(payload_size);
  memset(pending_payload_, 0, sizeof(pending_payload_));
  if (payload_size != 0U)
    memcpy(pending_payload_, payload, payload_size);

  job_ = Job::kAppend;
  phase_ = needs_activation ? Phase::kActivatePage : Phase::kRecordBody;
  flash_op_awaiting_completion_ = false;
  ++diagnostics_.append_started;
  return AppendResult::kStarted;
}

bool ObservationStore::takeAppendResult(bool& success, Handle& handle) {
  if (!append_result_ready_) return false;
  success = append_result_success_;
  handle = append_result_handle_;
  append_result_ready_ = false;
  return true;
}

ObservationStore::LookupResult ObservationStore::findRecord(
    const osf::RecordIdentity& identity, Record& out) const {
  if (!identity.valid()) return LookupResult::kNone;

  bool found = false;
  Record best;
  for (uint16_t page = osf::kControlPageCount; page < page_count_; ++page) {
    osf::PageInspection header;
    if (!readPage(page, header)) return LookupResult::kReadError;
    if (header.evidence == osf::PageEvidence::kErased ||
        header.evidence == osf::PageEvidence::kPrepared ||
        header.evidence == osf::PageEvidence::kStaged ||
        header.evidence == osf::PageEvidence::kPartialCommit ||
        header.evidence == osf::PageEvidence::kPartialActivation)
      continue;
    if (header.evidence != osf::PageEvidence::kActive ||
        header.kind != osf::PageKind::kData ||
        header.device_id != device_id_ ||
        header.incarnation != incarnation_)
      return LookupResult::kReadError;

    for (uint16_t slot = 0; slot < osf::kDataRecordsPerPage; ++slot) {
      osf::RecordInspection record;
      if (!readRecord(page, slot, record)) return LookupResult::kReadError;
      if (record.evidence == osf::RecordEvidence::kCorrupt)
        return LookupResult::kReadError;
      if (record.evidence != osf::RecordEvidence::kRetained &&
          record.evidence != osf::RecordEvidence::kReleased)
        continue;

      osf::RecordIdentity candidate;
      candidate.incarnation = record.incarnation;
      candidate.sequence = record.sequence;
      if (!sameIdentity(candidate, identity)) continue;
      if (found) return LookupResult::kReadError;

      found = true;
      best.handle.page = page;
      best.handle.slot = slot;
      best.handle.page_generation = header.generation;
      best.handle.identity = candidate;
      best.kind = record.kind;
      best.schema = record.schema;
      best.payload_size = record.payload_size;
      best.released = record.evidence == osf::RecordEvidence::kReleased;
      memcpy(best.payload, record.payload, sizeof(best.payload));
    }
  }

  if (!found) return LookupResult::kNone;
  out = best;
  return LookupResult::kFound;
}

ObservationStore::LookupResult ObservationStore::lookup(
    const osf::RecordIdentity& identity, Record& record) const {
  if (!ready_ || faulted_) return LookupResult::kReadError;
  return findRecord(identity, record);
}

ObservationStore::LookupResult ObservationStore::oldestRetained(
    Record& out) const {
  if (!ready_ || faulted_) return LookupResult::kReadError;

  bool found = false;
  Record best;
  for (uint16_t page = osf::kControlPageCount; page < page_count_; ++page) {
    osf::PageInspection header;
    if (!readPage(page, header)) return LookupResult::kReadError;
    if (header.evidence != osf::PageEvidence::kActive) continue;
    if (header.kind != osf::PageKind::kData ||
        header.device_id != device_id_ ||
        header.incarnation != incarnation_)
      return LookupResult::kReadError;

    for (uint16_t slot = 0; slot < osf::kDataRecordsPerPage; ++slot) {
      osf::RecordInspection record;
      if (!readRecord(page, slot, record)) return LookupResult::kReadError;
      if (record.evidence == osf::RecordEvidence::kCorrupt)
        return LookupResult::kReadError;
      if (record.evidence != osf::RecordEvidence::kRetained) continue;
      if (!found || record.sequence < best.handle.identity.sequence) {
        found = true;
        best.handle.page = page;
        best.handle.slot = slot;
        best.handle.page_generation = header.generation;
        best.handle.identity.incarnation = record.incarnation;
        best.handle.identity.sequence = record.sequence;
        best.kind = record.kind;
        best.schema = record.schema;
        best.payload_size = record.payload_size;
        best.released = false;
        memcpy(best.payload, record.payload, sizeof(best.payload));
      }
    }
  }

  if (!found) return LookupResult::kNone;
  out = best;
  return LookupResult::kFound;
}

bool ObservationStore::requestRelease(const osf::RecordIdentity& identity) {
  if (!ready_ || faulted_ || !identity.valid()) return false;
  if (busy() || append_result_ready_ || release_result_ready_ ||
      maintenance_result_ready_)
    return false;

  Record record;
  const LookupResult result = findRecord(identity, record);
  if (result != LookupResult::kFound) return false;

  if (record.released) {
    release_result_ready_ = true;
    release_result_success_ = true;
    return true;
  }

  osf::RecordInspection inspection;
  if (!readRecord(record.handle.page, record.handle.slot, inspection) ||
      inspection.evidence != osf::RecordEvidence::kRetained)
    return false;
  if (inspection.next_release_slot == UINT8_MAX) {
    ++diagnostics_.release_marker_exhausted;
    return false;
  }

  target_page_ = record.handle.page;
  target_slot_ = record.handle.slot;
  target_generation_ = record.handle.page_generation;
  pending_handle_ = record.handle;
  release_word_offset_ =
      inspection.next_release_slot == 0U
          ? osf::kDataRecordRelease0Offset
          : osf::kDataRecordRelease1Offset;
  job_ = Job::kRelease;
  phase_ = Phase::kReleaseMarker;
  flash_op_awaiting_completion_ = false;
  return true;
}

bool ObservationStore::takeReleaseResult(bool& success) {
  if (!release_result_ready_) return false;
  success = release_result_success_;
  release_result_ready_ = false;
  return true;
}

int ObservationStore::findErasedDataPage() const {
  for (uint16_t page = osf::kControlPageCount; page < page_count_; ++page) {
    osf::PageInspection inspection;
    if (!readPage(page, inspection)) return -1;
    if (inspection.evidence == osf::PageEvidence::kErased &&
        pageAllErased(page))
      return static_cast<int>(page);
  }
  return -1;
}

ObservationStore::MaintenanceResult ObservationStore::requestMaintenance() {
  if (!ready_ || faulted_) return MaintenanceResult::kRejected;
  if (busy() || append_result_ready_ || release_result_ready_ ||
      maintenance_result_ready_)
    return MaintenanceResult::kBusy;
  if (prepared_data_page_ >= 0) return MaintenanceResult::kNoWork;
  if (max_data_generation_ == UINT64_MAX)
    return MaintenanceResult::kRejected;

  const int page = findErasedDataPage();
  if (page < 0) return MaintenanceResult::kNoWork;

  target_page_ = static_cast<uint16_t>(page);
  target_slot_ = UINT16_MAX;
  target_generation_ = max_data_generation_ + 1U;
  osf::encodePageHeader(osf::PageKind::kData, target_generation_,
                        device_id_, incarnation_, page_blob_);
  job_ = Job::kMaintenance;
  phase_ = Phase::kHeaderBody;
  flash_op_awaiting_completion_ = false;
  return MaintenanceResult::kStarted;
}

bool ObservationStore::takeMaintenanceResult(bool& success) {
  if (!maintenance_result_ready_) return false;
  success = maintenance_result_success_;
  maintenance_result_ready_ = false;
  return true;
}

bool ObservationStore::countByRelease(bool released, uint32_t& count) const {
  count = 0;
  if (!ready_ || faulted_) return false;
  for (uint16_t page = osf::kControlPageCount; page < page_count_; ++page) {
    osf::PageInspection header;
    if (!readPage(page, header)) return false;
    if (header.evidence != osf::PageEvidence::kActive) continue;
    if (header.kind != osf::PageKind::kData ||
        header.device_id != device_id_ ||
        header.incarnation != incarnation_)
      return false;
    for (uint16_t slot = 0; slot < osf::kDataRecordsPerPage; ++slot) {
      osf::RecordInspection record;
      if (!readRecord(page, slot, record) ||
          record.evidence == osf::RecordEvidence::kCorrupt)
        return false;
      if ((!released && record.evidence == osf::RecordEvidence::kRetained) ||
          (released && record.evidence == osf::RecordEvidence::kReleased))
        ++count;
    }
  }
  return true;
}

bool ObservationStore::retainedCount(uint32_t& count) const {
  return countByRelease(false, count);
}

bool ObservationStore::releasedCount(uint32_t& count) const {
  return countByRelease(true, count);
}

FlashOpResult ObservationStore::programStep(
    uint32_t offset, const void* data, size_t size) {
  if (flash_op_awaiting_completion_) {
    const FlashOpResult result = flash_.pollPending();
    if (result != FlashOpResult::kPending)
      flash_op_awaiting_completion_ = false;
    return result;
  }
  const FlashOpResult result = flash_.program(offset, data, size);
  if (result == FlashOpResult::kPending)
    flash_op_awaiting_completion_ = true;
  return result;
}

void ObservationStore::finishAppend(bool success) {
  append_result_ready_ = true;
  append_result_success_ = success;
  append_result_handle_ = pending_handle_;
  if (success) {
    ++diagnostics_.append_committed;
    if (next_sequence_ != UINT32_MAX)
      ++next_sequence_;
    else
      next_sequence_ = 0U;
  } else {
    ++diagnostics_.append_failures;
  }
  job_ = Job::kNone;
  phase_ = Phase::kNone;
  flash_op_awaiting_completion_ = false;
}

void ObservationStore::finishRelease(bool success) {
  release_result_ready_ = true;
  release_result_success_ = success;
  if (success)
    ++diagnostics_.releases_committed;
  else
    ++diagnostics_.release_failures;
  job_ = Job::kNone;
  phase_ = Phase::kNone;
  flash_op_awaiting_completion_ = false;
}

void ObservationStore::finishMaintenance(bool success) {
  maintenance_result_ready_ = true;
  maintenance_result_success_ = success;
  job_ = Job::kNone;
  phase_ = Phase::kNone;
  flash_op_awaiting_completion_ = false;
}

void ObservationStore::failCurrentJob() {
  const Job failed = job_;
  const bool unreconciled = flash_.hasUnreconciledMutation();

  switch (failed) {
    case Job::kAppend:
      finishAppend(false);
      break;
    case Job::kRelease:
      finishRelease(false);
      break;
    case Job::kMaintenance:
      finishMaintenance(false);
      break;
    case Job::kNone:
      return;
  }

  if (unreconciled) {
    faulted_ = true;
    ++diagnostics_.unreconciled_mutation_faults;
    return;
  }

  const uint64_t saved_incarnation = incarnation_;
  if (!recover() || faulted_) {
    faulted_ = true;
    ++diagnostics_.recovery_faults;
    return;
  }

  // Before the first committed OBS1 header exists, the freshly generated
  // incarnation is RAM-authoritative for this boot. A torn first header must
  // not force a second random incarnation or permanently fault the store.
  if (incarnation_ == 0U && saved_incarnation != 0U)
    incarnation_ = saved_incarnation;
  else if (saved_incarnation != 0U && incarnation_ != saved_incarnation) {
    faulted_ = true;
    ++diagnostics_.recovery_faults;
  }
}

void ObservationStore::poll() {
  if (!ready_ || faulted_ || job_ == Job::kNone) return;
  static const uint32_t kZero = 0U;

  if (job_ == Job::kAppend) {
    if (phase_ == Phase::kActivatePage) {
      const uint32_t offset =
          pageOffset(target_page_) + osf::kPageHeaderActiveOffset;
      const FlashOpResult result =
          programStep(offset, &kZero, sizeof(kZero));
      if (result == FlashOpResult::kPending) return;

      uint8_t verify[4];
      const bool active =
          flash_.read(offset, verify, sizeof(verify)) &&
          osf::get32(verify) == osf::kActive &&
          pageGenerationMatches(target_page_, target_generation_);
      if (result == FlashOpResult::kFailed && !active)
        return failCurrentJob();
      if (!active) return failCurrentJob();

      active_data_page_ = target_page_;
      prepared_data_page_ = -1;
      ++diagnostics_.pages_activated;
      phase_ = Phase::kRecordBody;
      return;
    }

    if (phase_ == Phase::kRecordBody) {
      const uint32_t offset = recordOffset(target_page_, target_slot_);
      const FlashOpResult result =
          programStep(offset, record_blob_, osf::kDataRecordCommitOffset);
      if (result == FlashOpResult::kPending) return;

      uint8_t verify[osf::kDataRecordCommitOffset];
      const bool body_ok =
          flash_.read(offset, verify, sizeof(verify)) &&
          memcmp(verify, record_blob_, sizeof(verify)) == 0;
      if (result == FlashOpResult::kFailed && !body_ok)
        return failCurrentJob();
      if (!body_ok) return failCurrentJob();

      phase_ = Phase::kRecordCommit;
      return;
    }

    if (phase_ == Phase::kRecordCommit) {
      const uint32_t offset =
          recordOffset(target_page_, target_slot_) +
          osf::kDataRecordCommitOffset;
      const FlashOpResult result =
          programStep(offset,
                      record_blob_ + osf::kDataRecordCommitOffset, 4U);
      if (result == FlashOpResult::kPending) return;

      osf::RecordInspection record;
      const bool committed =
          pageGenerationMatches(target_page_, target_generation_) &&
          readRecord(target_page_, target_slot_, record) &&
          (record.evidence == osf::RecordEvidence::kRetained ||
           record.evidence == osf::RecordEvidence::kReleased) &&
          record.incarnation == pending_handle_.identity.incarnation &&
          record.sequence == pending_handle_.identity.sequence &&
          record.kind == pending_kind_ &&
          record.schema == pending_schema_ &&
          record.payload_size == pending_payload_size_ &&
          memcmp(record.payload, pending_payload_,
                 osf::kDataPayloadSize) == 0;
      if (result == FlashOpResult::kFailed && !committed)
        return failCurrentJob();
      if (!committed) return failCurrentJob();

      finishAppend(true);
      return;
    }
  }

  if (job_ == Job::kRelease && phase_ == Phase::kReleaseMarker) {
    const uint32_t offset =
        recordOffset(target_page_, target_slot_) + release_word_offset_;
    const FlashOpResult result =
        programStep(offset, &kZero, sizeof(kZero));
    if (result == FlashOpResult::kPending) return;

    osf::RecordInspection record;
    const bool released =
        pageGenerationMatches(target_page_, target_generation_) &&
        readRecord(target_page_, target_slot_, record) &&
        record.evidence == osf::RecordEvidence::kReleased &&
        record.incarnation == pending_handle_.identity.incarnation &&
        record.sequence == pending_handle_.identity.sequence;
    if (result == FlashOpResult::kFailed && !released)
      return failCurrentJob();
    if (!released) return failCurrentJob();

    finishRelease(true);
    return;
  }

  if (job_ == Job::kMaintenance) {
    if (phase_ == Phase::kHeaderBody) {
      const uint32_t offset = pageOffset(target_page_);
      const FlashOpResult result =
          programStep(offset, page_blob_, osf::kPageHeaderCommitOffset);
      if (result == FlashOpResult::kPending) return;

      uint8_t verify[osf::kPageHeaderCommitOffset];
      const bool body_ok =
          flash_.read(offset, verify, sizeof(verify)) &&
          memcmp(verify, page_blob_, sizeof(verify)) == 0;
      if (result == FlashOpResult::kFailed && !body_ok)
        return failCurrentJob();
      if (!body_ok) return failCurrentJob();

      phase_ = Phase::kHeaderCommit;
      return;
    }

    if (phase_ == Phase::kHeaderCommit) {
      const uint32_t offset =
          pageOffset(target_page_) + osf::kPageHeaderCommitOffset;
      const FlashOpResult result =
          programStep(offset,
                      page_blob_ + osf::kPageHeaderCommitOffset, 4U);
      if (result == FlashOpResult::kPending) return;

      osf::PageInspection header;
      const bool prepared =
          readPage(target_page_, header) &&
          header.evidence == osf::PageEvidence::kPrepared &&
          header.kind == osf::PageKind::kData &&
          header.generation == target_generation_ &&
          header.device_id == device_id_ &&
          header.incarnation == incarnation_ &&
          pagePayloadErased(target_page_);
      if (result == FlashOpResult::kFailed && !prepared)
        return failCurrentJob();
      if (!prepared) return failCurrentJob();

      prepared_data_page_ = target_page_;
      max_data_generation_ = target_generation_;
      ++diagnostics_.pages_prepared;
      finishMaintenance(true);
      return;
    }
  }

  failCurrentJob();
}

}  // namespace orun_tlp
