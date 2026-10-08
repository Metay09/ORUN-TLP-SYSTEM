#include "observation_store.h"

#include <string.h>

namespace orun_tlp {
namespace {

constexpr uint8_t kControlSchemaActive = 1U;
constexpr uint8_t kControlSchemaTombstone = 2U;

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

bool ObservationStore::readControl(
    uint16_t page, uint16_t slot, osf::ControlInspection& inspection) const {
  if (page >= osf::kControlPageCount ||
      slot >= osf::kControlRecordsPerPage)
    return false;
  uint8_t bytes[osf::kControlRecordSize];
  if (!flash_.read(controlOffset(page, slot), bytes, sizeof(bytes)))
    return false;
  return osf::inspectControl(bytes, sizeof(bytes), inspection);
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
  active_control_page_ = -1;
  active_control_generation_ = 0U;
  max_data_generation_ = 0U;
  next_sequence_ = 1U;
  next_control_serial_ = 1U;
  rotation_state_ = osc::StoreState();
  rotation_complete_state_ = osc::StoreState();
  rotation_control_slot_ = UINT16_MAX;
  rotation_state_serial_ = 0U;
  rotation_resuming_ = false;

  diagnostics_.recovered_retained = 0U;
  diagnostics_.recovered_released = 0U;
  diagnostics_.staged_records = 0U;
  diagnostics_.partial_record_commits = 0U;
  diagnostics_.control_recovered_active = 0U;
  diagnostics_.control_recovered_tombstones = 0U;
  diagnostics_.control_staged_records = 0U;
  diagnostics_.control_partial_commits = 0U;

  uint64_t recovered_incarnation = 0U;

  // Phase 1: choose the highest-generation ACTIVE control page. The other
  // control page is either the stale source of a completed compaction or a
  // non-authoritative interrupted target and may be erased on the next
  // control-maintenance cycle.
  for (uint16_t page = 0; page < osf::kControlPageCount; ++page) {
    osf::PageInspection header;
    if (!readPage(page, header)) return false;

    if (header.evidence == osf::PageEvidence::kErased ||
        header.evidence == osf::PageEvidence::kStaged ||
        header.evidence == osf::PageEvidence::kPrepared ||
        header.evidence == osf::PageEvidence::kPartialCommit ||
        header.evidence == osf::PageEvidence::kPartialActivation ||
        header.evidence == osf::PageEvidence::kCorrupt ||
        header.evidence == osf::PageEvidence::kUnsupported)
      continue;

    if (header.evidence != osf::PageEvidence::kActive ||
        header.kind != osf::PageKind::kControl ||
        header.device_id != device_id_ || header.incarnation == 0U) {
      faulted_ = true;
      ++diagnostics_.recovery_faults;
      continue;
    }

    if (active_control_page_ >= 0 &&
        header.generation == active_control_generation_) {
      faulted_ = true;
      ++diagnostics_.recovery_faults;
      continue;
    }

    if (header.generation > active_control_generation_) {
      active_control_generation_ = header.generation;
      active_control_page_ = page;
      recovered_incarnation = header.incarnation;
    }
  }

  if (faulted_) return true;

  // Phase 2: validate only the authoritative control page and recover the
  // latest StoreState. Older active control pages are stale compaction sources.
  if (active_control_page_ >= 0) {
    uint64_t serials[osf::kControlRecordsPerPage]{};
    uint16_t serial_count = 0U;
    uint64_t max_serial = 0U;

    for (uint16_t slot = 0; slot < osf::kControlRecordsPerPage; ++slot) {
      osf::ControlInspection control;
      if (!readControl(static_cast<uint16_t>(active_control_page_), slot,
                       control))
        return false;

      if (control.evidence == osf::ControlEvidence::kErased) continue;
      if (control.evidence == osf::ControlEvidence::kStaged) {
        ++diagnostics_.control_staged_records;
        continue;
      }
      if (control.evidence == osf::ControlEvidence::kPartialCommit) {
        ++diagnostics_.control_partial_commits;
        continue;
      }
      if (control.evidence == osf::ControlEvidence::kCorrupt ||
          !controlPayloadValid(control) ||
          (control.schema != kControlSchemaActive &&
           control.schema != kControlSchemaTombstone)) {
        faulted_ = true;
        ++diagnostics_.recovery_faults;
        continue;
      }
      if (control.kind == osf::ControlKind::kStoreState &&
          control.schema != kControlSchemaActive) {
        faulted_ = true;
        ++diagnostics_.recovery_faults;
        continue;
      }

      bool duplicate_serial = false;
      for (uint16_t i = 0; i < serial_count; ++i)
        if (serials[i] == control.serial) duplicate_serial = true;
      if (duplicate_serial || serial_count >= osf::kControlRecordsPerPage) {
        faulted_ = true;
        ++diagnostics_.recovery_faults;
        continue;
      }

      serials[serial_count++] = control.serial;
      if (control.serial > max_serial) max_serial = control.serial;
      if (control.schema == kControlSchemaTombstone ||
          control.evidence == osf::ControlEvidence::kCleared)
        ++diagnostics_.control_recovered_tombstones;
      else
        ++diagnostics_.control_recovered_active;
    }

    next_control_serial_ =
        max_serial == UINT64_MAX ? 0U : max_serial + 1U;

    osc::StoreState state;
    bool state_found = false;
    if (!readLatestStoreState(state, state_found)) return false;
    if (state_found) {
      diagnostics_.capacity_lost_total = state.capacity_lost_total;
      diagnostics_.capacity_lost_periodic = state.capacity_lost_periodic;
      diagnostics_.capacity_lost_event = state.capacity_lost_event;
      diagnostics_.capacity_lost_result = state.capacity_lost_result;

      if (state.rotation_pending) {
        if (state.target_page < osf::kControlPageCount ||
            state.target_page >= page_count_ ||
            state.target_new_generation <= state.target_old_generation) {
          faulted_ = true;
          ++diagnostics_.recovery_faults;
        } else {
          rotation_state_ = state;
          rotation_resuming_ = true;
          ++diagnostics_.rotation_recoveries;
        }
      }
    }
  }

  if (faulted_) return true;

  // Phase 3: recover data pages. A page named by a durable rotation intent may
  // be physically anywhere between old ACTIVE content and a newly PREPARED
  // replacement. That ambiguity is safe because the intent was committed
  // before erase and maintenance will deterministically re-erase/reprepare it.
  uint64_t generations[kMaxPages]{};
  uint16_t generation_count = 0U;
  PageSummary summaries[kMaxPages]{};
  uint16_t summary_count = 0U;
  uint32_t max_sequence = 0U;
  uint64_t active_generation = 0U;
  uint64_t prepared_generation = 0U;

  for (uint16_t page = osf::kControlPageCount; page < page_count_; ++page) {
    osf::PageInspection header;
    if (!readPage(page, header)) return false;

    const bool rotation_target =
        rotation_resuming_ && page == rotation_state_.target_page;

    if (rotation_target) {
      if (header.evidence == osf::PageEvidence::kActive) {
        if (header.kind != osf::PageKind::kData ||
            header.device_id != device_id_ ||
            header.incarnation == 0U ||
            (header.generation != rotation_state_.target_old_generation &&
             header.generation != rotation_state_.target_new_generation)) {
          faulted_ = true;
          ++diagnostics_.recovery_faults;
          continue;
        }
        if (header.generation == rotation_state_.target_new_generation) {
          // Current SF5C rotation never activates the replacement page before
          // completion state is committed. Seeing it ACTIVE would make the
          // ownership boundary ambiguous.
          faulted_ = true;
          ++diagnostics_.recovery_faults;
          continue;
        }
      } else if (header.evidence == osf::PageEvidence::kPrepared) {
        if (header.kind != osf::PageKind::kData ||
            header.device_id != device_id_ ||
            header.incarnation == 0U ||
            header.generation != rotation_state_.target_new_generation ||
            !pagePayloadErased(page)) {
          faulted_ = true;
          ++diagnostics_.recovery_faults;
        }
        continue;
      } else {
        // ERASED / staged / partial / corrupt header evidence is tolerated for
        // this one page only. The committed rotation intent is the authority
        // that permits maintenance to erase it again and resume.
        continue;
      }
    }

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
        header.evidence == osf::PageEvidence::kUnsupported ||
        header.kind != osf::PageKind::kData ||
        header.device_id != device_id_ ||
        header.incarnation == 0U) {
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
    uint32_t previous_sequence = 0U;

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

  if (rotation_resuming_ &&
      rotation_state_.target_new_generation > max_data_generation_)
    max_data_generation_ = rotation_state_.target_new_generation;

  if (faulted_) return true;

  // Data-page generations must preserve strictly increasing sequence ranges.
  uint64_t previous_generation = 0U;
  uint32_t previous_last_sequence = 0U;
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

  if (!rotation_resuming_ && prepared_data_page_ >= 0) {
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

  // A pending rotation blocks normal append preparation until maintenance has
  // completed the durable intent -> erase -> replacement -> completion chain.
  if (rotation_resuming_)
    prepared_data_page_ = -1;

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
  control_write_result_ready_ = false;
  control_maintenance_result_ready_ = false;
  device_id_ = device_id;
  incarnation_ = 0;
  next_sequence_ = 1U;
  next_control_serial_ = 1U;
  max_data_generation_ = 0;
  active_data_page_ = -1;
  prepared_data_page_ = -1;
  active_control_page_ = -1;
  active_control_generation_ = 0U;

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
  if (!ready_ || faulted_ || busy() || rotation_resuming_ ||
      next_sequence_ == 0U || incarnation_ == 0U)
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
  if (rotation_resuming_ || busy() || append_result_ready_ ||
      release_result_ready_ || maintenance_result_ready_ ||
      control_write_result_ready_ || control_maintenance_result_ready_)
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

int ObservationStore::findEmptyControlSlot() const {
  if (active_control_page_ < 0) return -1;
  for (uint16_t slot = 0; slot < osf::kControlRecordsPerPage; ++slot) {
    osf::ControlInspection control;
    if (!readControl(static_cast<uint16_t>(active_control_page_), slot,
                     control))
      return -1;
    if (control.evidence == osf::ControlEvidence::kErased)
      return static_cast<int>(slot);
  }
  return -1;
}

uint16_t ObservationStore::emptyControlSlots(bool& read_ok) const {
  read_ok = false;
  if (active_control_page_ < 0) {
    read_ok = true;
    return 0U;
  }
  uint16_t count = 0U;
  for (uint16_t slot = 0; slot < osf::kControlRecordsPerPage; ++slot) {
    osf::ControlInspection control;
    if (!readControl(static_cast<uint16_t>(active_control_page_), slot,
                     control))
      return 0U;
    if (control.evidence == osf::ControlEvidence::kErased)
      ++count;
  }
  read_ok = true;
  return count;
}

bool ObservationStore::readLatestStoreState(
    osc::StoreState& state, bool& found) const {
  state = osc::StoreState();
  found = false;
  if (active_control_page_ < 0) return true;

  uint64_t best_serial = 0U;
  for (uint16_t slot = 0; slot < osf::kControlRecordsPerPage; ++slot) {
    osf::ControlInspection control;
    if (!readControl(static_cast<uint16_t>(active_control_page_), slot,
                     control))
      return false;
    if (control.evidence == osf::ControlEvidence::kErased ||
        control.evidence == osf::ControlEvidence::kStaged ||
        control.evidence == osf::ControlEvidence::kPartialCommit)
      continue;
    if (control.evidence == osf::ControlEvidence::kCorrupt ||
        !controlPayloadValid(control))
      return false;
    if (control.kind != osf::ControlKind::kStoreState ||
        control.schema != kControlSchemaActive)
      continue;
    if (!found || control.serial > best_serial) {
      if (!osc::decodeStoreState(control.payload, control.payload_size, state))
        return false;
      best_serial = control.serial;
      found = true;
    }
  }
  return true;
}

int ObservationStore::findOldestDataPage() const {
  int oldest_any = -1;
  uint64_t oldest_any_generation = UINT64_MAX;
  int oldest_fully_released = -1;
  uint64_t oldest_released_generation = UINT64_MAX;

  for (uint16_t page = osf::kControlPageCount; page < page_count_; ++page) {
    osf::PageInspection header;
    if (!readPage(page, header)) return -1;
    if (header.evidence != osf::PageEvidence::kActive) continue;
    if (header.kind != osf::PageKind::kData ||
        header.device_id != device_id_ ||
        header.incarnation != incarnation_)
      return -1;

    bool has_retained = false;
    for (uint16_t slot = 0; slot < osf::kDataRecordsPerPage; ++slot) {
      osf::RecordInspection record;
      if (!readRecord(page, slot, record)) return -1;
      if (record.evidence == osf::RecordEvidence::kCorrupt)
        return -1;
      if (record.evidence == osf::RecordEvidence::kRetained)
        has_retained = true;
    }

    if (header.generation < oldest_any_generation) {
      oldest_any_generation = header.generation;
      oldest_any = static_cast<int>(page);
    }
    if (!has_retained &&
        header.generation < oldest_released_generation) {
      oldest_released_generation = header.generation;
      oldest_fully_released = static_cast<int>(page);
    }
  }

  // Product rule: reclaim responsibility-free history before intentionally
  // losing retained history. If every page still owns retained data, fall back
  // to strict oldest-generation eviction.
  return oldest_fully_released >= 0 ? oldest_fully_released : oldest_any;
}

bool ObservationStore::buildRotationIntent(
    uint16_t page, uint64_t new_generation,
    osc::StoreState& state) const {
  state = osc::StoreState();
  if (page < osf::kControlPageCount || page >= page_count_ ||
      new_generation == 0U)
    return false;

  osf::PageInspection header;
  if (!readPage(page, header) ||
      header.evidence != osf::PageEvidence::kActive ||
      header.kind != osf::PageKind::kData ||
      header.device_id != device_id_ ||
      header.incarnation != incarnation_ ||
      header.generation == 0U ||
      new_generation <= header.generation)
    return false;

  state.capacity_lost_total = diagnostics_.capacity_lost_total;
  state.capacity_lost_periodic = diagnostics_.capacity_lost_periodic;
  state.capacity_lost_event = diagnostics_.capacity_lost_event;
  state.capacity_lost_result = diagnostics_.capacity_lost_result;
  state.rotation_pending = true;
  state.target_page = page;
  state.target_old_generation = header.generation;
  state.target_new_generation = new_generation;

  for (uint16_t slot = 0; slot < osf::kDataRecordsPerPage; ++slot) {
    osf::RecordInspection record;
    if (!readRecord(page, slot, record)) return false;
    if (record.evidence == osf::RecordEvidence::kErased ||
        record.evidence == osf::RecordEvidence::kStaged ||
        record.evidence == osf::RecordEvidence::kPartialCommit)
      continue;
    if (record.evidence == osf::RecordEvidence::kCorrupt ||
        record.incarnation != incarnation_ || record.sequence == 0U)
      return false;

    if (state.first_retired_sequence == 0U)
      state.first_retired_sequence = record.sequence;
    state.last_retired_sequence = record.sequence;

    if (record.evidence != osf::RecordEvidence::kRetained)
      continue;

    if (state.capacity_lost_total == UINT32_MAX)
      return false;
    ++state.capacity_lost_total;
    switch (record.kind) {
      case osf::RecordKind::kPeriodic:
        if (state.capacity_lost_periodic == UINT32_MAX) return false;
        ++state.capacity_lost_periodic;
        break;
      case osf::RecordKind::kEvent:
        if (state.capacity_lost_event == UINT32_MAX) return false;
        ++state.capacity_lost_event;
        break;
      case osf::RecordKind::kResult:
        if (state.capacity_lost_result == UINT32_MAX) return false;
        ++state.capacity_lost_result;
        break;
    }
  }

  return state.capacity_lost_total ==
         state.capacity_lost_periodic +
             state.capacity_lost_event +
             state.capacity_lost_result;
}

bool ObservationStore::controlPayloadValid(
    const osf::ControlInspection& control) const {
  if (control.schema != kControlSchemaActive &&
      control.schema != kControlSchemaTombstone)
    return false;

  switch (control.kind) {
    case osf::ControlKind::kExactObject: {
      if (control.payload_size != osc::kExactObjectPayloadSize) return false;
      osc::ExactObject value;
      return osc::decodeExactObject(control.payload, control.payload_size,
                                    value);
    }
    case osf::ControlKind::kOpenOccurrence: {
      if (control.payload_size != osc::kOpenOccurrencePayloadSize)
        return false;
      osc::OpenOccurrence value;
      return osc::decodeOpenOccurrence(control.payload, control.payload_size,
                                       value);
    }
    case osf::ControlKind::kResultGuard: {
      if (control.payload_size != osc::kResultGuardPayloadSize) return false;
      osc::ResultGuard value;
      return osc::decodeResultGuard(control.payload, control.payload_size,
                                    value);
    }
    case osf::ControlKind::kStoreState: {
      if (control.schema != kControlSchemaActive ||
          control.payload_size != osc::kStoreStatePayloadSize)
        return false;
      osc::StoreState value;
      return osc::decodeStoreState(control.payload, control.payload_size,
                                   value);
    }
  }
  return false;
}

bool ObservationStore::controlSameKey(
    const osf::ControlInspection& a,
    const osf::ControlInspection& b) const {
  if (a.kind != b.kind) return false;

  switch (a.kind) {
    case osf::ControlKind::kExactObject: {
      osc::ExactObject av;
      osc::ExactObject bv;
      return osc::decodeExactObject(a.payload, a.payload_size, av) &&
             osc::decodeExactObject(b.payload, b.payload_size, bv) &&
             osc::sameExactObjectKey(av, bv);
    }
    case osf::ControlKind::kOpenOccurrence: {
      osc::OpenOccurrence av;
      osc::OpenOccurrence bv;
      return osc::decodeOpenOccurrence(a.payload, a.payload_size, av) &&
             osc::decodeOpenOccurrence(b.payload, b.payload_size, bv) &&
             osc::sameOpenOccurrenceKey(av, bv);
    }
    case osf::ControlKind::kResultGuard: {
      osc::ResultGuard av;
      osc::ResultGuard bv;
      return osc::decodeResultGuard(a.payload, a.payload_size, av) &&
             osc::decodeResultGuard(b.payload, b.payload_size, bv) &&
             osc::sameResultGuardKey(av, bv);
    }
    case osf::ControlKind::kStoreState:
      return true;
  }
  return false;
}

bool ObservationStore::controlIsLatest(
    uint16_t slot, const osf::ControlInspection& control,
    bool& read_ok) const {
  read_ok = false;
  if (active_control_page_ < 0 ||
      (control.evidence != osf::ControlEvidence::kActive &&
       control.evidence != osf::ControlEvidence::kCleared) ||
      !controlPayloadValid(control))
    return false;

  for (uint16_t other = 0; other < osf::kControlRecordsPerPage; ++other) {
    if (other == slot) continue;
    osf::ControlInspection candidate;
    if (!readControl(static_cast<uint16_t>(active_control_page_), other,
                     candidate))
      return false;
    if (candidate.evidence != osf::ControlEvidence::kActive &&
        candidate.evidence != osf::ControlEvidence::kCleared)
      continue;
    if (!controlPayloadValid(candidate)) return false;
    if (candidate.serial > control.serial &&
        controlSameKey(control, candidate)) {
      read_ok = true;
      return false;
    }
  }
  read_ok = true;
  return true;
}

ObservationStore::ControlLookupResult ObservationStore::findLatestControl(
    osf::ControlKind kind, const uint8_t* key_payload, size_t key_size,
    osf::ControlInspection& out) const {
  out = osf::ControlInspection();
  if (!ready_ || faulted_ || active_control_page_ < 0 ||
      key_payload == nullptr || key_size == 0U ||
      key_size > osf::kControlPayloadSize)
    return active_control_page_ < 0
               ? ControlLookupResult::kNone
               : ControlLookupResult::kReadError;

  osf::ControlInspection key;
  key.evidence = osf::ControlEvidence::kActive;
  key.kind = kind;
  key.schema = kControlSchemaActive;
  key.payload_size = static_cast<uint16_t>(key_size);
  memcpy(key.payload, key_payload, key_size);
  if (!controlPayloadValid(key)) return ControlLookupResult::kReadError;

  bool found = false;
  osf::ControlInspection best;
  for (uint16_t slot = 0; slot < osf::kControlRecordsPerPage; ++slot) {
    osf::ControlInspection candidate;
    if (!readControl(static_cast<uint16_t>(active_control_page_), slot,
                     candidate))
      return ControlLookupResult::kReadError;
    if (candidate.evidence == osf::ControlEvidence::kErased ||
        candidate.evidence == osf::ControlEvidence::kStaged ||
        candidate.evidence == osf::ControlEvidence::kPartialCommit)
      continue;
    if (candidate.evidence == osf::ControlEvidence::kCorrupt ||
        !controlPayloadValid(candidate))
      return ControlLookupResult::kReadError;
    if (candidate.kind != kind || !controlSameKey(key, candidate)) continue;
    if (!found || candidate.serial > best.serial) {
      best = candidate;
      found = true;
    }
  }

  if (!found ||
      best.evidence == osf::ControlEvidence::kCleared ||
      best.schema == kControlSchemaTombstone)
    return ControlLookupResult::kNone;
  out = best;
  return ControlLookupResult::kFound;
}

uint16_t ObservationStore::activeControlCount(
    osf::ControlKind kind, bool& read_ok) const {
  read_ok = false;
  if (active_control_page_ < 0) {
    read_ok = true;
    return 0U;
  }

  uint16_t count = 0U;
  for (uint16_t slot = 0; slot < osf::kControlRecordsPerPage; ++slot) {
    osf::ControlInspection control;
    if (!readControl(static_cast<uint16_t>(active_control_page_), slot,
                     control))
      return 0U;
    if (control.evidence == osf::ControlEvidence::kErased ||
        control.evidence == osf::ControlEvidence::kStaged ||
        control.evidence == osf::ControlEvidence::kPartialCommit)
      continue;
    if (control.evidence == osf::ControlEvidence::kCorrupt ||
        !controlPayloadValid(control))
      return 0U;
    if (control.kind == kind &&
        control.evidence == osf::ControlEvidence::kActive &&
        control.schema == kControlSchemaActive) {
      bool latest_read_ok = false;
      const bool latest = controlIsLatest(slot, control, latest_read_ok);
      if (!latest_read_ok) return 0U;
      if (latest) {
        if (kind == osf::ControlKind::kExactObject) {
          osc::ExactObject exact;
          if (!osc::decodeExactObject(control.payload,
                                      control.payload_size, exact))
            return 0U;
          Record backing;
          const LookupResult backing_result = findRecord(exact.identity, backing);
          if (backing_result == LookupResult::kReadError) return 0U;
          if (backing_result == LookupResult::kNone) continue;
        }
        ++count;
      }
    }
  }
  read_ok = true;
  return count;
}

ObservationStore::ControlWriteResult ObservationStore::requestControlWrite(
    osf::ControlKind kind, uint8_t schema,
    const uint8_t* payload, size_t payload_size) {
  if (!ready_ || faulted_ || next_control_serial_ == 0U ||
      payload == nullptr ||
      payload_size == 0U || payload_size > osf::kControlPayloadSize ||
      (schema != kControlSchemaActive &&
       schema != kControlSchemaTombstone))
    return ControlWriteResult::kRejected;
  if (active_control_page_ < 0)
    return ControlWriteResult::kNoCapacity;
  if (rotation_resuming_)
    return ControlWriteResult::kBusy;
  if (busy() || append_result_ready_ || release_result_ready_ ||
      maintenance_result_ready_ || control_write_result_ready_ ||
      control_maintenance_result_ready_)
    return ControlWriteResult::kBusy;
  if (kind == osf::ControlKind::kStoreState &&
      schema != kControlSchemaActive)
    return ControlWriteResult::kRejected;

  const int slot = findEmptyControlSlot();
  if (slot < 0) return ControlWriteResult::kNoCapacity;

  osf::ControlInspection semantic;
  semantic.evidence = osf::ControlEvidence::kActive;
  semantic.kind = kind;
  semantic.schema = schema;
  semantic.payload_size = static_cast<uint16_t>(payload_size);
  semantic.serial = next_control_serial_;
  memcpy(semantic.payload, payload, payload_size);
  if (!controlPayloadValid(semantic)) return ControlWriteResult::kRejected;

  if (!osf::encodeControl(kind, schema, next_control_serial_,
                          payload, payload_size, control_blob_))
    return ControlWriteResult::kRejected;

  target_page_ = static_cast<uint16_t>(active_control_page_);
  target_slot_ = static_cast<uint16_t>(slot);
  target_generation_ = active_control_generation_;
  pending_control_kind_ = kind;
  pending_control_schema_ = schema;
  pending_control_payload_size_ = static_cast<uint16_t>(payload_size);
  pending_control_serial_ = next_control_serial_;
  memset(pending_control_payload_, 0, sizeof(pending_control_payload_));
  memcpy(pending_control_payload_, payload, payload_size);

  job_ = Job::kControlWrite;
  phase_ = Phase::kControlBody;
  flash_op_awaiting_completion_ = false;
  ++diagnostics_.control_writes_started;
  return ControlWriteResult::kStarted;
}

bool ObservationStore::prepareNextControlCopy() {
  if (control_source_page_ >= osf::kControlPageCount) {
    faulted_ = true;
    return false;
  }

  while (control_source_scan_slot_ < osf::kControlRecordsPerPage) {
    const uint16_t source_slot = control_source_scan_slot_++;
    osf::ControlInspection control;
    if (!readControl(control_source_page_, source_slot, control)) {
      faulted_ = true;
      return false;
    }
    if (control.evidence == osf::ControlEvidence::kErased ||
        control.evidence == osf::ControlEvidence::kStaged ||
        control.evidence == osf::ControlEvidence::kPartialCommit)
      continue;
    if (control.evidence == osf::ControlEvidence::kCorrupt ||
        !controlPayloadValid(control)) {
      faulted_ = true;
      return false;
    }

    bool latest_read_ok = false;
    const bool latest =
        controlIsLatest(source_slot, control, latest_read_ok);
    if (!latest_read_ok) {
      faulted_ = true;
      return false;
    }
    bool logically_active =
        control.evidence == osf::ControlEvidence::kActive &&
        control.schema == kControlSchemaActive && latest;
    if (logically_active &&
        control.kind == osf::ControlKind::kExactObject) {
      osc::ExactObject exact;
      if (!osc::decodeExactObject(control.payload,
                                  control.payload_size, exact)) {
        faulted_ = true;
        return false;
      }
      Record backing;
      const LookupResult backing_result = findRecord(exact.identity, backing);
      if (backing_result == LookupResult::kReadError) {
        faulted_ = true;
        return false;
      }
      if (backing_result == LookupResult::kNone)
        logically_active = false;
    }
    if (!logically_active) continue;

    if (control_target_slot_ >= osf::kControlRecordsPerPage) {
      faulted_ = true;
      return false;
    }

    if (!osf::encodeControl(control.kind, control.schema, control.serial,
                            control.payload, control.payload_size,
                            control_blob_)) {
      faulted_ = true;
      return false;
    }
    pending_control_kind_ = control.kind;
    pending_control_schema_ = control.schema;
    pending_control_payload_size_ = control.payload_size;
    pending_control_serial_ = control.serial;
    memset(pending_control_payload_, 0, sizeof(pending_control_payload_));
    memcpy(pending_control_payload_, control.payload, control.payload_size);
    return true;
  }
  return false;
}

bool ObservationStore::controlCopyFinished() const {
  return control_source_scan_slot_ >= osf::kControlRecordsPerPage;
}

ObservationStore::ControlLookupResult ObservationStore::findExactObject(
    const osf::RecordIdentity& identity, osc::ExactObject& value) const {
  value = osc::ExactObject();
  if (!identity.valid()) return ControlLookupResult::kReadError;
  osc::ExactObject probe;
  probe.identity = identity;
  probe.record_kind = osf::RecordKind::kPeriodic;
  probe.object_size = 1U;
  probe.object[0] = 0U;
  uint8_t payload[osc::kExactObjectPayloadSize];
  if (!osc::encodeExactObject(probe, payload))
    return ControlLookupResult::kReadError;
  osf::ControlInspection control;
  const ControlLookupResult result =
      findLatestControl(osf::ControlKind::kExactObject,
                        payload, sizeof(payload), control);
  if (result != ControlLookupResult::kFound) return result;
  if (!osc::decodeExactObject(control.payload, control.payload_size, value))
    return ControlLookupResult::kReadError;

  Record backing;
  const LookupResult backing_result = findRecord(value.identity, backing);
  if (backing_result == LookupResult::kReadError)
    return ControlLookupResult::kReadError;
  if (backing_result == LookupResult::kNone)
    return ControlLookupResult::kNone;
  return ControlLookupResult::kFound;
}

ObservationStore::ControlWriteResult ObservationStore::requestPutExactObject(
    const osc::ExactObject& value) {
  uint8_t payload[osc::kExactObjectPayloadSize];
  if (!osc::encodeExactObject(value, payload))
    return ControlWriteResult::kRejected;

  Record backing;
  const LookupResult backing_result = findRecord(value.identity, backing);
  if (backing_result != LookupResult::kFound || backing.released)
    return ControlWriteResult::kRejected;

  osf::ControlInspection existing;
  const ControlLookupResult lookup =
      findLatestControl(osf::ControlKind::kExactObject,
                        payload, sizeof(payload), existing);
  if (lookup == ControlLookupResult::kReadError)
    return ControlWriteResult::kRejected;
  if (lookup == ControlLookupResult::kFound) {
    if (existing.payload_size == sizeof(payload) &&
        memcmp(existing.payload, payload, sizeof(payload)) == 0)
      return ControlWriteResult::kAlreadySatisfied;
    return ControlWriteResult::kRejected;
  }

  bool read_ok = false;
  if (activeControlCount(osf::ControlKind::kExactObject, read_ok) >=
          osf::kMaxExactCustodyObjects ||
      !read_ok)
    return read_ok ? ControlWriteResult::kNoCapacity
                   : ControlWriteResult::kRejected;
  return requestControlWrite(osf::ControlKind::kExactObject,
                             kControlSchemaActive,
                             payload, sizeof(payload));
}

ObservationStore::ControlWriteResult ObservationStore::requestClearExactObject(
    const osf::RecordIdentity& identity) {
  osc::ExactObject existing;
  const ControlLookupResult lookup = findExactObject(identity, existing);
  if (lookup == ControlLookupResult::kReadError)
    return ControlWriteResult::kRejected;
  if (lookup == ControlLookupResult::kNone)
    return ControlWriteResult::kAlreadySatisfied;
  uint8_t payload[osc::kExactObjectPayloadSize];
  if (!osc::encodeExactObject(existing, payload))
    return ControlWriteResult::kRejected;
  return requestControlWrite(osf::ControlKind::kExactObject,
                             kControlSchemaTombstone,
                             payload, sizeof(payload));
}

ObservationStore::ControlLookupResult ObservationStore::findOpenOccurrence(
    const osc::OpenOccurrence& key, osc::OpenOccurrence& value) const {
  value = osc::OpenOccurrence();
  osc::OpenOccurrence probe = key;
  if (probe.occurrence_id == 0U) probe.occurrence_id = 1U;
  uint8_t payload[osc::kOpenOccurrencePayloadSize];
  if (!osc::encodeOpenOccurrence(probe, payload))
    return ControlLookupResult::kReadError;
  osf::ControlInspection control;
  const ControlLookupResult result =
      findLatestControl(osf::ControlKind::kOpenOccurrence,
                        payload, sizeof(payload), control);
  if (result != ControlLookupResult::kFound) return result;
  if (!osc::decodeOpenOccurrence(control.payload, control.payload_size, value))
    return ControlLookupResult::kReadError;
  return ControlLookupResult::kFound;
}

ObservationStore::ControlWriteResult
ObservationStore::requestPutOpenOccurrence(
    const osc::OpenOccurrence& value) {
  uint8_t payload[osc::kOpenOccurrencePayloadSize];
  if (!osc::encodeOpenOccurrence(value, payload))
    return ControlWriteResult::kRejected;

  osf::ControlInspection existing;
  const ControlLookupResult lookup =
      findLatestControl(osf::ControlKind::kOpenOccurrence,
                        payload, sizeof(payload), existing);
  if (lookup == ControlLookupResult::kReadError)
    return ControlWriteResult::kRejected;
  if (lookup == ControlLookupResult::kFound) {
    if (existing.payload_size == sizeof(payload) &&
        memcmp(existing.payload, payload, sizeof(payload)) == 0)
      return ControlWriteResult::kAlreadySatisfied;
    return ControlWriteResult::kRejected;
  }

  bool read_ok = false;
  if (activeControlCount(osf::ControlKind::kOpenOccurrence, read_ok) >=
          osf::kMaxOpenOccurrences ||
      !read_ok)
    return read_ok ? ControlWriteResult::kNoCapacity
                   : ControlWriteResult::kRejected;
  return requestControlWrite(osf::ControlKind::kOpenOccurrence,
                             kControlSchemaActive,
                             payload, sizeof(payload));
}

ObservationStore::ControlWriteResult
ObservationStore::requestClearOpenOccurrence(
    const osc::OpenOccurrence& key) {
  osc::OpenOccurrence existing;
  const ControlLookupResult lookup = findOpenOccurrence(key, existing);
  if (lookup == ControlLookupResult::kReadError)
    return ControlWriteResult::kRejected;
  if (lookup == ControlLookupResult::kNone)
    return ControlWriteResult::kAlreadySatisfied;
  uint8_t payload[osc::kOpenOccurrencePayloadSize];
  if (!osc::encodeOpenOccurrence(existing, payload))
    return ControlWriteResult::kRejected;
  return requestControlWrite(osf::ControlKind::kOpenOccurrence,
                             kControlSchemaTombstone,
                             payload, sizeof(payload));
}

ObservationStore::ControlLookupResult ObservationStore::findResultGuard(
    const osc::ResultGuard& key, osc::ResultGuard& value) const {
  value = osc::ResultGuard();
  osc::ResultGuard probe = key;
  if (probe.opcode == 0U) probe.opcode = 1U;
  if (!probe.result_identity.valid()) {
    probe.result_identity.incarnation =
        incarnation_ == 0U ? 1U : incarnation_;
    probe.result_identity.sequence = 1U;
  }
  uint8_t payload[osc::kResultGuardPayloadSize];
  if (!osc::encodeResultGuard(probe, payload))
    return ControlLookupResult::kReadError;
  osf::ControlInspection control;
  const ControlLookupResult result =
      findLatestControl(osf::ControlKind::kResultGuard,
                        payload, sizeof(payload), control);
  if (result != ControlLookupResult::kFound) return result;
  if (!osc::decodeResultGuard(control.payload, control.payload_size, value))
    return ControlLookupResult::kReadError;
  return ControlLookupResult::kFound;
}

ObservationStore::ControlWriteResult ObservationStore::requestPutResultGuard(
    const osc::ResultGuard& value) {
  uint8_t payload[osc::kResultGuardPayloadSize];
  if (!osc::encodeResultGuard(value, payload))
    return ControlWriteResult::kRejected;

  osf::ControlInspection existing;
  const ControlLookupResult lookup =
      findLatestControl(osf::ControlKind::kResultGuard,
                        payload, sizeof(payload), existing);
  if (lookup == ControlLookupResult::kReadError)
    return ControlWriteResult::kRejected;
  if (lookup == ControlLookupResult::kFound) {
    if (existing.payload_size == sizeof(payload) &&
        memcmp(existing.payload, payload, sizeof(payload)) == 0)
      return ControlWriteResult::kAlreadySatisfied;
    // Same authenticated authority context + command_id but a different
    // canonical request/result binding is a fail-closed conflict.
    return ControlWriteResult::kRejected;
  }

  bool read_ok = false;
  if (activeControlCount(osf::ControlKind::kResultGuard, read_ok) >=
          osf::kMaxResultGuards ||
      !read_ok)
    return read_ok ? ControlWriteResult::kNoCapacity
                   : ControlWriteResult::kRejected;
  return requestControlWrite(osf::ControlKind::kResultGuard,
                             kControlSchemaActive,
                             payload, sizeof(payload));
}

ObservationStore::ControlWriteResult ObservationStore::requestClearResultGuard(
    const osc::ResultGuard& key) {
  osc::ResultGuard existing;
  const ControlLookupResult lookup = findResultGuard(key, existing);
  if (lookup == ControlLookupResult::kReadError)
    return ControlWriteResult::kRejected;
  if (lookup == ControlLookupResult::kNone)
    return ControlWriteResult::kAlreadySatisfied;
  uint8_t payload[osc::kResultGuardPayloadSize];
  if (!osc::encodeResultGuard(existing, payload))
    return ControlWriteResult::kRejected;
  return requestControlWrite(osf::ControlKind::kResultGuard,
                             kControlSchemaTombstone,
                             payload, sizeof(payload));
}

bool ObservationStore::takeControlWriteResult(bool& success) {
  if (!control_write_result_ready_) return false;
  success = control_write_result_success_;
  control_write_result_ready_ = false;
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
    const bool rotation_target =
        rotation_resuming_ && page == rotation_state_.target_page;
    if (rotation_target) {
      if (header.evidence != osf::PageEvidence::kActive ||
          header.kind != osf::PageKind::kData ||
          header.device_id != device_id_ ||
          header.incarnation != incarnation_ ||
          header.generation != rotation_state_.target_old_generation)
        continue;
    } else {
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
    }

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
  if (rotation_resuming_ || busy() || append_result_ready_ ||
      release_result_ready_ || maintenance_result_ready_ ||
      control_write_result_ready_ || control_maintenance_result_ready_)
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

int ObservationStore::findReclaimableDataPage() const {
  for (uint16_t page = osf::kControlPageCount; page < page_count_; ++page) {
    osf::PageInspection inspection;
    if (!readPage(page, inspection)) return -1;
    const bool orphan_header =
        inspection.evidence == osf::PageEvidence::kStaged ||
        inspection.evidence == osf::PageEvidence::kPartialCommit ||
        inspection.evidence == osf::PageEvidence::kPartialActivation;
    if (orphan_header && pagePayloadErased(page))
      return static_cast<int>(page);
  }
  return -1;
}

ObservationStore::MaintenanceResult ObservationStore::requestMaintenance() {
  if (!ready_ || faulted_) return MaintenanceResult::kRejected;
  if (busy() || append_result_ready_ || release_result_ready_ ||
      maintenance_result_ready_ || control_write_result_ready_ ||
      control_maintenance_result_ready_)
    return MaintenanceResult::kBusy;

  if (max_data_generation_ == UINT64_MAX)
    return MaintenanceResult::kRejected;

  // Resume a previously committed rotation intent before allowing any other
  // data-store mutation. One free control slot is needed for the completion
  // StoreState record; control compaction may be requested first if necessary.
  if (rotation_resuming_) {
    if (active_control_page_ < 0 || next_control_serial_ == 0U)
      return MaintenanceResult::kRejected;
    bool read_ok = false;
    const uint16_t empty = emptyControlSlots(read_ok);
    if (!read_ok) return MaintenanceResult::kRejected;
    if (empty < 1U)
      return MaintenanceResult::kControlMaintenanceRequired;

    target_page_ = rotation_state_.target_page;
    target_generation_ = rotation_state_.target_new_generation;
    target_slot_ = UINT16_MAX;
    osf::encodePageHeader(osf::PageKind::kData, target_generation_,
                          device_id_, incarnation_, page_blob_);

    rotation_complete_state_ = rotation_state_;
    rotation_complete_state_.rotation_pending = false;
    rotation_complete_state_.target_page = UINT16_MAX;
    rotation_complete_state_.target_old_generation = 0U;
    rotation_complete_state_.target_new_generation = 0U;

    rotation_control_slot_ =
        static_cast<uint16_t>(findEmptyControlSlot());
    if (rotation_control_slot_ == UINT16_MAX)
      return MaintenanceResult::kControlMaintenanceRequired;
    rotation_state_serial_ = next_control_serial_;

    job_ = Job::kMaintenance;
    phase_ = Phase::kRotationErasePage;
    flash_op_awaiting_completion_ = false;
    return MaintenanceResult::kStarted;
  }

  if (prepared_data_page_ >= 0) return MaintenanceResult::kNoWork;

  const int erased_page = findErasedDataPage();
  if (erased_page >= 0) {
    target_page_ = static_cast<uint16_t>(erased_page);
    target_slot_ = UINT16_MAX;
    target_generation_ = max_data_generation_ + 1U;
    osf::encodePageHeader(osf::PageKind::kData, target_generation_,
                          device_id_, incarnation_, page_blob_);
    job_ = Job::kMaintenance;
    phase_ = Phase::kHeaderBody;
    flash_op_awaiting_completion_ = false;
    return MaintenanceResult::kStarted;
  }

  // A torn never-authoritative page header is safe to erase/reuse without a
  // capacity-loss intent because no ACTIVE data ownership was established.
  const int orphan_page = findReclaimableDataPage();
  if (orphan_page >= 0) {
    target_page_ = static_cast<uint16_t>(orphan_page);
    target_slot_ = UINT16_MAX;
    target_generation_ = max_data_generation_ + 1U;
    osf::encodePageHeader(osf::PageKind::kData, target_generation_,
                          device_id_, incarnation_, page_blob_);
    job_ = Job::kMaintenance;
    phase_ = Phase::kHeaderErasePage;
    flash_op_awaiting_completion_ = false;
    return MaintenanceResult::kStarted;
  }

  // If the newest ACTIVE page still has a free record slot, there is no
  // capacity pressure yet. Do not evict history merely because no erased page
  // is currently available for pre-preparation.
  uint16_t append_page = 0U;
  uint16_t append_slot = 0U;
  bool append_needs_activation = false;
  if (findAppendSlot(append_page, append_slot, append_needs_activation) &&
      !append_needs_activation)
    return MaintenanceResult::kNoWork;

  // No erased data page remains. Rotation is permitted only after a durable
  // StoreState intent can be appended with one additional slot reserved for
  // completion. This preserves oldest-first loss diagnostics across power cut.
  if (active_control_page_ < 0 || next_control_serial_ == 0U)
    return MaintenanceResult::kControlMaintenanceRequired;
  // A fresh rotation needs two serials: durable intent and durable completion.
  if (next_control_serial_ == UINT64_MAX)
    return MaintenanceResult::kRejected;

  bool read_ok = false;
  const uint16_t empty = emptyControlSlots(read_ok);
  if (!read_ok) return MaintenanceResult::kRejected;
  if (empty < 2U)
    return MaintenanceResult::kControlMaintenanceRequired;

  const int oldest = findOldestDataPage();
  if (oldest < 0) return MaintenanceResult::kRejected;

  const uint64_t new_generation = max_data_generation_ + 1U;
  if (!buildRotationIntent(static_cast<uint16_t>(oldest),
                           new_generation, rotation_state_))
    return MaintenanceResult::kRejected;

  uint8_t state_payload[osc::kStoreStatePayloadSize];
  if (!osc::encodeStoreState(rotation_state_, state_payload))
    return MaintenanceResult::kRejected;

  const int control_slot = findEmptyControlSlot();
  if (control_slot < 0) return MaintenanceResult::kControlMaintenanceRequired;
  rotation_control_slot_ = static_cast<uint16_t>(control_slot);
  rotation_state_serial_ = next_control_serial_;
  if (!osf::encodeControl(osf::ControlKind::kStoreState,
                          kControlSchemaActive,
                          rotation_state_serial_,
                          state_payload, sizeof(state_payload),
                          control_blob_))
    return MaintenanceResult::kRejected;

  pending_control_kind_ = osf::ControlKind::kStoreState;
  pending_control_schema_ = kControlSchemaActive;
  pending_control_payload_size_ =
      static_cast<uint16_t>(sizeof(state_payload));
  pending_control_serial_ = rotation_state_serial_;
  memset(pending_control_payload_, 0, sizeof(pending_control_payload_));
  memcpy(pending_control_payload_, state_payload, sizeof(state_payload));

  target_page_ = static_cast<uint16_t>(oldest);
  target_generation_ = new_generation;
  target_slot_ = UINT16_MAX;
  osf::encodePageHeader(osf::PageKind::kData, target_generation_,
                        device_id_, incarnation_, page_blob_);

  rotation_complete_state_ = rotation_state_;
  rotation_complete_state_.rotation_pending = false;
  rotation_complete_state_.target_page = UINT16_MAX;
  rotation_complete_state_.target_old_generation = 0U;
  rotation_complete_state_.target_new_generation = 0U;

  job_ = Job::kMaintenance;
  phase_ = Phase::kRotationIntentBody;
  flash_op_awaiting_completion_ = false;
  return MaintenanceResult::kStarted;
}

bool ObservationStore::takeMaintenanceResult(bool& success) {
  if (!maintenance_result_ready_) return false;
  success = maintenance_result_success_;
  maintenance_result_ready_ = false;
  return true;
}

ObservationStore::MaintenanceResult
ObservationStore::requestControlMaintenance() {
  if (!ready_ || faulted_) return MaintenanceResult::kRejected;
  if (busy() || append_result_ready_ || release_result_ready_ ||
      maintenance_result_ready_ || control_write_result_ready_ ||
      control_maintenance_result_ready_)
    return MaintenanceResult::kBusy;

  if (active_control_page_ >= 0) {
    bool read_ok = false;
    const uint16_t empty = emptyControlSlots(read_ok);
    if (!read_ok) return MaintenanceResult::kRejected;
    const uint16_t reserve = rotation_resuming_ ? 1U : 2U;
    if (empty >= reserve) return MaintenanceResult::kNoWork;
  }

  if (active_control_page_ >= 0 &&
      active_control_generation_ == UINT64_MAX)
    return MaintenanceResult::kRejected;

  if (active_control_page_ < 0) {
    target_page_ = 0U;
    control_source_page_ = UINT16_MAX;
    target_generation_ = 1U;
  } else {
    target_page_ =
        static_cast<uint16_t>(1 - active_control_page_);
    control_source_page_ =
        static_cast<uint16_t>(active_control_page_);
    target_generation_ = active_control_generation_ + 1U;
  }

  target_slot_ = UINT16_MAX;
  control_source_scan_slot_ = 0U;
  control_target_slot_ = 0U;
  osf::encodePageHeader(osf::PageKind::kControl, target_generation_,
                        device_id_, incarnation_, page_blob_);

  job_ = Job::kControlMaintenance;
  phase_ = pageAllErased(target_page_)
               ? Phase::kControlHeaderBody
               : Phase::kControlEraseTarget;
  flash_op_awaiting_completion_ = false;
  return MaintenanceResult::kStarted;
}

bool ObservationStore::takeControlMaintenanceResult(bool& success) {
  if (!control_maintenance_result_ready_) return false;
  success = control_maintenance_result_success_;
  control_maintenance_result_ready_ = false;
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

FlashOpResult ObservationStore::eraseStep(uint16_t page) {
  if (flash_op_awaiting_completion_) {
    const FlashOpResult result = flash_.pollPending();
    if (result != FlashOpResult::kPending)
      flash_op_awaiting_completion_ = false;
    return result;
  }
  const FlashOpResult result = flash_.erasePage(page);
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

void ObservationStore::finishControlWrite(bool success) {
  control_write_result_ready_ = true;
  control_write_result_success_ = success;
  if (success) {
    ++diagnostics_.control_writes_committed;
    if (next_control_serial_ != UINT64_MAX)
      ++next_control_serial_;
    else
      next_control_serial_ = 0U;
  } else {
    ++diagnostics_.control_write_failures;
  }
  job_ = Job::kNone;
  phase_ = Phase::kNone;
  flash_op_awaiting_completion_ = false;
}

void ObservationStore::finishControlMaintenance(bool success) {
  control_maintenance_result_ready_ = true;
  control_maintenance_result_success_ = success;
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
    case Job::kControlWrite:
      finishControlWrite(false);
      break;
    case Job::kControlMaintenance:
      finishControlMaintenance(false);
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
    if (phase_ == Phase::kHeaderErasePage) {
      const FlashOpResult result = eraseStep(target_page_);
      if (result == FlashOpResult::kPending) return;
      const bool erased = pageAllErased(target_page_);
      if (result == FlashOpResult::kFailed && !erased)
        return failCurrentJob();
      if (!erased) return failCurrentJob();
      phase_ = Phase::kHeaderBody;
      return;
    }

    if (phase_ == Phase::kRotationIntentBody) {
      if (active_control_page_ < 0 ||
          rotation_control_slot_ >= osf::kControlRecordsPerPage)
        return failCurrentJob();

      const uint32_t offset =
          controlOffset(static_cast<uint16_t>(active_control_page_),
                        rotation_control_slot_);
      const FlashOpResult result =
          programStep(offset, control_blob_, osf::kControlRecordCommitOffset);
      if (result == FlashOpResult::kPending) return;

      uint8_t verify[osf::kControlRecordCommitOffset];
      const bool body_ok =
          flash_.read(offset, verify, sizeof(verify)) &&
          memcmp(verify, control_blob_, sizeof(verify)) == 0;
      if (result == FlashOpResult::kFailed && !body_ok)
        return failCurrentJob();
      if (!body_ok) return failCurrentJob();

      phase_ = Phase::kRotationIntentCommit;
      return;
    }

    if (phase_ == Phase::kRotationIntentCommit) {
      const uint16_t control_page =
          static_cast<uint16_t>(active_control_page_);
      const uint32_t offset =
          controlOffset(control_page, rotation_control_slot_) +
          osf::kControlRecordCommitOffset;
      const FlashOpResult result =
          programStep(offset,
                      control_blob_ + osf::kControlRecordCommitOffset, 4U);
      if (result == FlashOpResult::kPending) return;

      osf::ControlInspection control;
      const bool committed =
          readControl(control_page, rotation_control_slot_, control) &&
          control.evidence == osf::ControlEvidence::kActive &&
          control.kind == osf::ControlKind::kStoreState &&
          control.schema == kControlSchemaActive &&
          control.serial == rotation_state_serial_ &&
          control.payload_size == osc::kStoreStatePayloadSize &&
          memcmp(control.payload, pending_control_payload_,
                 osf::kControlPayloadSize) == 0;
      if (result == FlashOpResult::kFailed && !committed)
        return failCurrentJob();
      if (!committed) return failCurrentJob();

      rotation_resuming_ = true;
      diagnostics_.capacity_lost_total =
          rotation_state_.capacity_lost_total;
      diagnostics_.capacity_lost_periodic =
          rotation_state_.capacity_lost_periodic;
      diagnostics_.capacity_lost_event =
          rotation_state_.capacity_lost_event;
      diagnostics_.capacity_lost_result =
          rotation_state_.capacity_lost_result;

      if (rotation_state_serial_ == UINT64_MAX)
        return failCurrentJob();
      next_control_serial_ = rotation_state_serial_ + 1U;
      const int completion_slot = findEmptyControlSlot();
      if (completion_slot < 0) return failCurrentJob();
      rotation_control_slot_ =
          static_cast<uint16_t>(completion_slot);
      rotation_state_serial_ = next_control_serial_;

      phase_ = Phase::kRotationErasePage;
      return;
    }

    if (phase_ == Phase::kRotationErasePage) {
      const FlashOpResult result = eraseStep(target_page_);
      if (result == FlashOpResult::kPending) return;

      const bool erased = pageAllErased(target_page_);
      if (result == FlashOpResult::kFailed && !erased)
        return failCurrentJob();
      if (!erased) return failCurrentJob();

      phase_ = Phase::kRotationHeaderBody;
      return;
    }

    if (phase_ == Phase::kRotationHeaderBody) {
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

      phase_ = Phase::kRotationHeaderCommit;
      return;
    }

    if (phase_ == Phase::kRotationHeaderCommit) {
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

      uint8_t state_payload[osc::kStoreStatePayloadSize];
      if (!osc::encodeStoreState(rotation_complete_state_, state_payload))
        return failCurrentJob();
      if (!osf::encodeControl(osf::ControlKind::kStoreState,
                              kControlSchemaActive,
                              rotation_state_serial_,
                              state_payload, sizeof(state_payload),
                              control_blob_))
        return failCurrentJob();

      pending_control_kind_ = osf::ControlKind::kStoreState;
      pending_control_schema_ = kControlSchemaActive;
      pending_control_payload_size_ =
          static_cast<uint16_t>(sizeof(state_payload));
      pending_control_serial_ = rotation_state_serial_;
      memset(pending_control_payload_, 0, sizeof(pending_control_payload_));
      memcpy(pending_control_payload_, state_payload, sizeof(state_payload));

      phase_ = Phase::kRotationCompleteBody;
      return;
    }

    if (phase_ == Phase::kRotationCompleteBody) {
      if (active_control_page_ < 0 ||
          rotation_control_slot_ >= osf::kControlRecordsPerPage)
        return failCurrentJob();

      const uint32_t offset =
          controlOffset(static_cast<uint16_t>(active_control_page_),
                        rotation_control_slot_);
      const FlashOpResult result =
          programStep(offset, control_blob_, osf::kControlRecordCommitOffset);
      if (result == FlashOpResult::kPending) return;

      uint8_t verify[osf::kControlRecordCommitOffset];
      const bool body_ok =
          flash_.read(offset, verify, sizeof(verify)) &&
          memcmp(verify, control_blob_, sizeof(verify)) == 0;
      if (result == FlashOpResult::kFailed && !body_ok)
        return failCurrentJob();
      if (!body_ok) return failCurrentJob();

      phase_ = Phase::kRotationCompleteCommit;
      return;
    }

    if (phase_ == Phase::kRotationCompleteCommit) {
      const uint16_t control_page =
          static_cast<uint16_t>(active_control_page_);
      const uint32_t offset =
          controlOffset(control_page, rotation_control_slot_) +
          osf::kControlRecordCommitOffset;
      const FlashOpResult result =
          programStep(offset,
                      control_blob_ + osf::kControlRecordCommitOffset, 4U);
      if (result == FlashOpResult::kPending) return;

      osf::ControlInspection control;
      const bool committed =
          readControl(control_page, rotation_control_slot_, control) &&
          control.evidence == osf::ControlEvidence::kActive &&
          control.kind == osf::ControlKind::kStoreState &&
          control.schema == kControlSchemaActive &&
          control.serial == rotation_state_serial_ &&
          control.payload_size == osc::kStoreStatePayloadSize &&
          memcmp(control.payload, pending_control_payload_,
                 osf::kControlPayloadSize) == 0;
      if (result == FlashOpResult::kFailed && !committed)
        return failCurrentJob();
      if (!committed) return failCurrentJob();

      diagnostics_.capacity_lost_total =
          rotation_complete_state_.capacity_lost_total;
      diagnostics_.capacity_lost_periodic =
          rotation_complete_state_.capacity_lost_periodic;
      diagnostics_.capacity_lost_event =
          rotation_complete_state_.capacity_lost_event;
      diagnostics_.capacity_lost_result =
          rotation_complete_state_.capacity_lost_result;
      ++diagnostics_.pages_reclaimed;

      if (rotation_state_serial_ == UINT64_MAX)
        next_control_serial_ = 0U;
      else
        next_control_serial_ = rotation_state_serial_ + 1U;

      rotation_state_ = rotation_complete_state_;
      rotation_resuming_ = false;
      rotation_control_slot_ = UINT16_MAX;
      rotation_state_serial_ = 0U;
      finishMaintenance(true);
      return;
    }

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

  if (job_ == Job::kControlWrite) {
    if (phase_ == Phase::kControlBody) {
      const uint32_t offset = controlOffset(target_page_, target_slot_);
      const FlashOpResult result =
          programStep(offset, control_blob_, osf::kControlRecordCommitOffset);
      if (result == FlashOpResult::kPending) return;

      uint8_t verify[osf::kControlRecordCommitOffset];
      const bool body_ok =
          flash_.read(offset, verify, sizeof(verify)) &&
          memcmp(verify, control_blob_, sizeof(verify)) == 0;
      if (result == FlashOpResult::kFailed && !body_ok)
        return failCurrentJob();
      if (!body_ok) return failCurrentJob();

      phase_ = Phase::kControlCommit;
      return;
    }

    if (phase_ == Phase::kControlCommit) {
      const uint32_t offset =
          controlOffset(target_page_, target_slot_) +
          osf::kControlRecordCommitOffset;
      const FlashOpResult result =
          programStep(offset,
                      control_blob_ + osf::kControlRecordCommitOffset, 4U);
      if (result == FlashOpResult::kPending) return;

      osf::ControlInspection control;
      const bool committed =
          readControl(target_page_, target_slot_, control) &&
          control.evidence == osf::ControlEvidence::kActive &&
          control.kind == pending_control_kind_ &&
          control.schema == pending_control_schema_ &&
          control.serial == pending_control_serial_ &&
          control.payload_size == pending_control_payload_size_ &&
          memcmp(control.payload, pending_control_payload_,
                 osf::kControlPayloadSize) == 0;
      if (result == FlashOpResult::kFailed && !committed)
        return failCurrentJob();
      if (!committed) return failCurrentJob();

      finishControlWrite(true);
      return;
    }
  }

  if (job_ == Job::kControlMaintenance) {
    if (phase_ == Phase::kControlEraseTarget) {
      const FlashOpResult result = eraseStep(target_page_);
      if (result == FlashOpResult::kPending) return;
      const bool erased = pageAllErased(target_page_);
      if (result == FlashOpResult::kFailed && !erased)
        return failCurrentJob();
      if (!erased) return failCurrentJob();
      phase_ = Phase::kControlHeaderBody;
      return;
    }

    if (phase_ == Phase::kControlHeaderBody) {
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
      phase_ = Phase::kControlHeaderCommit;
      return;
    }

    if (phase_ == Phase::kControlHeaderCommit) {
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
          header.kind == osf::PageKind::kControl &&
          header.generation == target_generation_ &&
          header.device_id == device_id_ &&
          header.incarnation == incarnation_;
      if (result == FlashOpResult::kFailed && !prepared)
        return failCurrentJob();
      if (!prepared) return failCurrentJob();

      control_target_slot_ = 0U;
      control_source_scan_slot_ = 0U;
      if (control_source_page_ == UINT16_MAX) {
        phase_ = Phase::kControlActivate;
        return;
      }

      if (prepareNextControlCopy()) {
        phase_ = Phase::kControlCopyBody;
        return;
      }
      if (faulted_) return failCurrentJob();
      if (controlCopyFinished()) {
        phase_ = Phase::kControlActivate;
        return;
      }
      return failCurrentJob();
    }

    if (phase_ == Phase::kControlCopyBody) {
      const uint32_t offset =
          controlOffset(target_page_, control_target_slot_);
      const FlashOpResult result =
          programStep(offset, control_blob_, osf::kControlRecordCommitOffset);
      if (result == FlashOpResult::kPending) return;

      uint8_t verify[osf::kControlRecordCommitOffset];
      const bool body_ok =
          flash_.read(offset, verify, sizeof(verify)) &&
          memcmp(verify, control_blob_, sizeof(verify)) == 0;
      if (result == FlashOpResult::kFailed && !body_ok)
        return failCurrentJob();
      if (!body_ok) return failCurrentJob();
      phase_ = Phase::kControlCopyCommit;
      return;
    }

    if (phase_ == Phase::kControlCopyCommit) {
      const uint32_t offset =
          controlOffset(target_page_, control_target_slot_) +
          osf::kControlRecordCommitOffset;
      const FlashOpResult result =
          programStep(offset,
                      control_blob_ + osf::kControlRecordCommitOffset, 4U);
      if (result == FlashOpResult::kPending) return;

      osf::ControlInspection control;
      const bool committed =
          readControl(target_page_, control_target_slot_, control) &&
          control.evidence == osf::ControlEvidence::kActive &&
          control.kind == pending_control_kind_ &&
          control.schema == pending_control_schema_ &&
          control.serial == pending_control_serial_ &&
          control.payload_size == pending_control_payload_size_ &&
          memcmp(control.payload, pending_control_payload_,
                 osf::kControlPayloadSize) == 0;
      if (result == FlashOpResult::kFailed && !committed)
        return failCurrentJob();
      if (!committed) return failCurrentJob();

      ++control_target_slot_;
      if (prepareNextControlCopy()) {
        phase_ = Phase::kControlCopyBody;
        return;
      }
      if (faulted_) return failCurrentJob();
      if (controlCopyFinished()) {
        phase_ = Phase::kControlActivate;
        return;
      }
      return failCurrentJob();
    }

    if (phase_ == Phase::kControlActivate) {
      const uint32_t offset =
          pageOffset(target_page_) + osf::kPageHeaderActiveOffset;
      const FlashOpResult result =
          programStep(offset, &kZero, sizeof(kZero));
      if (result == FlashOpResult::kPending) return;

      osf::PageInspection header;
      const bool active =
          readPage(target_page_, header) &&
          header.evidence == osf::PageEvidence::kActive &&
          header.kind == osf::PageKind::kControl &&
          header.generation == target_generation_ &&
          header.device_id == device_id_ &&
          header.incarnation == incarnation_;
      if (result == FlashOpResult::kFailed && !active)
        return failCurrentJob();
      if (!active) return failCurrentJob();

      if (control_source_page_ != UINT16_MAX)
        ++diagnostics_.control_pages_compacted;
      active_control_page_ = target_page_;
      active_control_generation_ = target_generation_;
      finishControlMaintenance(true);
      return;
    }
  }

  failCurrentJob();
}

}  // namespace orun_tlp
