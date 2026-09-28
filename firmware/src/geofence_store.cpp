#include "geofence_store.h"

#include <string.h>

#include "journal_format.h"
#include "storage_config.h"

namespace orun_tlp {
namespace {

constexpr unsigned kPageCount = storage_config::kGeofenceRegionPages;
static_assert(kPageCount == 2, "GeofenceStore A/B requires exactly two pages");

bool evidenceCommitted(geofence_format::PageEvidence evidence) {
  return evidence == geofence_format::PageEvidence::kCommittedClear ||
         evidence == geofence_format::PageEvidence::kCommittedConfigured;
}

bool evidenceSafeUncommitted(geofence_format::PageEvidence evidence) {
  return evidence == geofence_format::PageEvidence::kErased ||
         evidence == geofence_format::PageEvidence::kUncommittedOrTorn;
}

GeofenceResourceState resourceStateOf(
    const geofence_format::Snapshot& snapshot) {
  return snapshot.state == geofence_format::ResourceState::kClear
             ? GeofenceResourceState::kClear
             : GeofenceResourceState::kConfigured;
}

bool exactSuccessor(const geofence_format::Record& committed,
                    const geofence_format::Record& staged) {
  return committed.generation != UINT64_MAX &&
         committed.token.revision != UINT32_MAX &&
         staged.generation == committed.generation + 1 &&
         staged.token.incarnation == committed.token.incarnation &&
         staged.token.revision == committed.token.revision + 1;
}

bool readPageTailErased(FlashBackend& flash, unsigned page,
                        bool& tail_erased) {
  constexpr size_t kChunkSize = 64;
  uint8_t chunk[kChunkSize];
  uint32_t within_page = geofence_format::kRecordSize;
  tail_erased = true;

  while (within_page < storage_config::kPageSize) {
    const uint32_t remaining = storage_config::kPageSize - within_page;
    const size_t size =
        remaining < kChunkSize ? static_cast<size_t>(remaining) : kChunkSize;
    const uint32_t offset =
        page * storage_config::kPageSize + within_page;
    if (!flash.read(offset, chunk, size)) return false;
    if (!journal_format::erased(chunk, size)) {
      tail_erased = false;
      return true;
    }
    within_page += static_cast<uint32_t>(size);
  }
  return true;
}

}  // namespace

bool GeofenceStore::sameSnapshot(const geofence_format::Snapshot& a,
                                 const geofence_format::Snapshot& b) {
  if (a.state != b.state || a.area_count != b.area_count ||
      a.total_vertex_count != b.total_vertex_count)
    return false;
  for (uint8_t i = 0; i < geofence_format::kMaximumAreas; ++i)
    if (a.area_vertex_counts[i] != b.area_vertex_counts[i]) return false;
  for (uint16_t i = 0; i < geofence_format::kMaximumTotalVertices; ++i) {
    if (a.vertices[i].latitude_e7 != b.vertices[i].latitude_e7 ||
        a.vertices[i].longitude_e7 != b.vertices[i].longitude_e7)
      return false;
  }
  return true;
}

void GeofenceStore::clearRecoveredRuntimeState() {
  snapshot_ = geofence_format::Snapshot();
  token_ = {};
  resource_state_ = GeofenceResourceState::kUnavailable;
  token_state_ = GeofenceTokenState::kUnavailable;
  semantic_unambiguous_ = false;
  maintenance_reset_required_ = false;
  generation_ = 0;
  active_page_ = -1;
}

void GeofenceStore::setUnavailableMaintenance(
    GeofenceTokenState token_state) {
  snapshot_ = geofence_format::Snapshot();
  token_ = {};
  resource_state_ = GeofenceResourceState::kUnavailable;
  token_state_ = token_state;
  semantic_unambiguous_ = false;
  maintenance_reset_required_ = true;
  generation_ = 0;
  active_page_ = -1;
  ++diagnostics_.maintenance_lockouts;
}

void GeofenceStore::setSemanticFallback(
    const geofence_format::Record& record,
    GeofenceTokenState token_state) {
  snapshot_ = record.snapshot;
  token_ = record.token;  // evidence only; stateToken hides unless VALID.
  resource_state_ = resourceStateOf(record.snapshot);
  token_state_ = token_state;
  semantic_unambiguous_ = true;
  maintenance_reset_required_ = true;
  generation_ = record.generation;
  active_page_ = -1;
  ++diagnostics_.maintenance_lockouts;
}

bool GeofenceStore::begin() {
  ready_ = false;
  job_ = Job::kNone;
  blob_step_ = BlobStep::kBody;
  flash_op_awaiting_completion_ = false;
  diagnostics_ = {};
  mutation_result_ready_ = false;
  mutation_success_ = false;
  mutation_unreconciled_ = false;
  recovery_pending_ = false;
  clearRecoveredRuntimeState();

  if (!flash_.begin()) return false;
  if (!recover()) return false;

  // A blank partition is not semantically CLEAR until a real, committed
  // baseline exists. With entropy available, create that baseline now while
  // the current production contract still runs pre-SoftDevice.
  if (!maintenance_reset_required_ && active_page_ < 0 &&
      resource_state_ == GeofenceResourceState::kUnavailable &&
      token_state_ == GeofenceTokenState::kUnavailable) {
    if (incarnation_source_ != nullptr && !establishFreshBaseline())
      return false;
  }

  ready_ = true;
  return true;
}

bool GeofenceStore::recover() {
  RecoveredPage* pages = recovery_pages_;
  bool all_erased = true;
  bool any_unsupported = false;
  bool any_corrupt = false;
  unsigned committed_count = 0;
  int committed_pages[kPageCount] = {-1, -1};

  for (unsigned page = 0; page < kPageCount; ++page) {
    pages[page] = RecoveredPage{};
    if (!flash_.read(page * storage_config::kPageSize, scratch_,
                     sizeof(scratch_)))
      return false;
    if (!geofence_format::inspectPage(scratch_, sizeof(scratch_),
                                      pages[page].inspection))
      return false;

    if (pages[page].inspection.evidence !=
        geofence_format::PageEvidence::kUnsupportedNewer) {
      bool tail_erased = false;
      if (!readPageTailErased(flash_, page, tail_erased)) return false;
      pages[page].tail_dirty = !tail_erased;
    }

    const auto evidence = pages[page].inspection.evidence;
    if (evidence != geofence_format::PageEvidence::kErased ||
        pages[page].tail_dirty)
      all_erased = false;

    if (evidence == geofence_format::PageEvidence::kUnsupportedNewer) {
      any_unsupported = true;
      continue;
    }

    if (evidenceCommitted(evidence)) {
      committed_pages[committed_count++] = static_cast<int>(page);
      if (pages[page].tail_dirty) any_corrupt = true;
      continue;
    }

    if (pages[page].tail_dirty ||
        evidence == geofence_format::PageEvidence::kPartialCommit ||
        evidence == geofence_format::PageEvidence::kCommittedCorrupt ||
        evidence == geofence_format::PageEvidence::kSupportedCorrupt) {
      any_corrupt = true;
      ++diagnostics_.recovery_corruptions;
    }
  }

  clearRecoveredRuntimeState();

  if (all_erased) return true;

  if (any_unsupported) {
    setUnavailableMaintenance(GeofenceTokenState::kUncertain);
    return true;
  }

  if (committed_count == 0) {
    // Staged or torn bytes are not authority. Never infer CLEAR from erased or
    // uncommitted evidence and never auto-erase it.
    setUnavailableMaintenance(
        any_corrupt ? GeofenceTokenState::kUncertain
                    : GeofenceTokenState::kUnavailable);
    return true;
  }

  if (committed_count == 2) {
    const auto& a = pages[committed_pages[0]].inspection.record;
    const auto& b = pages[committed_pages[1]].inspection.record;

    if (a.token.incarnation != b.token.incarnation) {
      // Cross-incarnation committed authority is contradictory by contract.
      setUnavailableMaintenance(GeofenceTokenState::kUncertain);
      return true;
    }

    const geofence_format::Record* high = &a;
    const geofence_format::Record* low = &b;
    if (b.generation > a.generation) {
      high = &b;
      low = &a;
    }

    const bool exact_lineage =
        low->generation != UINT64_MAX &&
        high->generation == low->generation + 1 &&
        low->token.revision != UINT32_MAX &&
        high->token.revision == low->token.revision + 1;

    if (!exact_lineage || any_corrupt) {
      // Exact lineage still tells us which semantic snapshot is newer even
      // when reserved-tail evidence invalidates CAS authority. Preserve that
      // read-only semantic value while forcing token UNCERTAIN. Without exact
      // lineage, only identical semantics are safe to preserve.
      if (exact_lineage || sameSnapshot(high->snapshot, low->snapshot))
        setSemanticFallback(*high, GeofenceTokenState::kUncertain);
      else
        setUnavailableMaintenance(GeofenceTokenState::kUncertain);
      return true;
    }

    const int active =
        pages[committed_pages[1]].inspection.record.generation >
                pages[committed_pages[0]].inspection.record.generation
            ? committed_pages[1]
            : committed_pages[0];
    const auto& record = pages[active].inspection.record;
    active_page_ = active;
    generation_ = record.generation;
    snapshot_ = record.snapshot;
    token_ = record.token;
    resource_state_ = resourceStateOf(record.snapshot);
    token_state_ = GeofenceTokenState::kValid;
    semantic_unambiguous_ = true;
    return true;
  }

  const int committed_page = committed_pages[0];
  const int other_page = 1 - committed_page;
  const auto& committed = pages[committed_page].inspection.record;
  const auto& other = pages[other_page].inspection;

  if (pages[committed_page].tail_dirty || any_corrupt) {
    setSemanticFallback(committed, GeofenceTokenState::kUncertain);
    return true;
  }

  if (other.evidence == geofence_format::PageEvidence::kStaged) {
    if (!other.has_decoded_record ||
        !exactSuccessor(committed, other.record)) {
      setSemanticFallback(committed, GeofenceTokenState::kUncertain);
      return true;
    }
    // Body+CRC is fully verified and commit remains erased: the candidate is
    // definitely non-authoritative, so the prior committed token remains VALID.
  } else if (!evidenceSafeUncommitted(other.evidence)) {
    setSemanticFallback(committed, GeofenceTokenState::kUncertain);
    return true;
  }

  active_page_ = committed_page;
  generation_ = committed.generation;
  snapshot_ = committed.snapshot;
  token_ = committed.token;
  resource_state_ = resourceStateOf(committed.snapshot);
  token_state_ = GeofenceTokenState::kValid;
  semantic_unambiguous_ = true;
  return true;
}

bool GeofenceStore::writeFreshBaseline(
    const geofence_format::Record& record) {
  if (!geofence_format::encode(record, blob_, sizeof(blob_))) return false;

  const uint32_t offset = 0;
  constexpr size_t kBodyAndCrcSize = geofence_format::kCrcOffset + 4U;
  const FlashOpResult body =
      flash_.program(offset, blob_, kBodyAndCrcSize);
  if (body != FlashOpResult::kDone) return false;

  if (!flash_.read(offset, scratch_, kBodyAndCrcSize) ||
      memcmp(scratch_, blob_, kBodyAndCrcSize) != 0)
    return false;

  const FlashOpResult commit =
      flash_.program(offset + geofence_format::kCommitOffset,
                     blob_ + geofence_format::kCommitOffset, 4);
  if (commit != FlashOpResult::kDone) return false;

  if (!flash_.read(offset, scratch_, sizeof(scratch_)) ||
      memcmp(scratch_, blob_, sizeof(scratch_)) != 0)
    return false;
  return true;
}

bool GeofenceStore::establishFreshBaseline() {
  if (incarnation_source_ == nullptr) return true;

  uint64_t incarnation = 0;
  if (!incarnation_source_->generate(incarnation) || incarnation == 0)
    return true;

  geofence_format::Record record;
  record.generation = 1;
  record.token = geofence_format::StateToken(incarnation, 1);
  geofence_format::makeClearSnapshot(record.snapshot);

  if (!writeFreshBaseline(record)) {
    ++diagnostics_.baseline_failures;
    if (flash_.hasUnreconciledMutation()) {
      mutation_unreconciled_ = true;
      token_state_ = GeofenceTokenState::kUncertain;
      ++diagnostics_.unreconciled_mutation_faults;
      return true;
    }
    return recover();
  }

  active_page_ = 0;
  generation_ = 1;
  snapshot_ = record.snapshot;
  token_ = record.token;
  resource_state_ = GeofenceResourceState::kClear;
  token_state_ = GeofenceTokenState::kValid;
  semantic_unambiguous_ = true;
  maintenance_reset_required_ = false;
  ++diagnostics_.baseline_commits;
  return true;
}

bool GeofenceStore::requestReplace(const GeofenceAreaSetView& area_set) {
  geofence_format::Snapshot candidate;
  if (!geofence_format::canonicalizeConfiguredAreaSet(area_set, candidate)) {
    ++diagnostics_.rejected_candidates;
    return false;
  }
  return requestSnapshot(candidate);
}

bool GeofenceStore::requestClear() {
  geofence_format::Snapshot clear;
  geofence_format::makeClearSnapshot(clear);
  return requestSnapshot(clear);
}

bool GeofenceStore::requestSnapshot(
    const geofence_format::Snapshot& candidate) {
  if (!ready_ || busy() || mutation_unreconciled_ || recovery_pending_)
    return false;

  if (mutation_result_ready_) {
    ++diagnostics_.blocked_pending_result;
    return false;
  }

  if (maintenance_reset_required_ ||
      token_state_ != GeofenceTokenState::kValid ||
      active_page_ < 0 ||
      resource_state_ == GeofenceResourceState::kUnavailable) {
    ++diagnostics_.maintenance_lockouts;
    return false;
  }

  if (sameSnapshot(candidate, snapshot_)) {
    if (!semantic_unambiguous_) return false;
    ++diagnostics_.skipped_unchanged;
    return true;
  }

  return startMutation(candidate);
}

bool GeofenceStore::startMutation(
    const geofence_format::Snapshot& candidate) {
  if (generation_ == UINT64_MAX || token_.revision == UINT32_MAX)
    return false;

  target_page_ = 1U - static_cast<unsigned>(active_page_);
  pending_generation_ = generation_ + 1;
  pending_snapshot_ = candidate;
  pending_token_ = geofence_format::StateToken(
      token_.incarnation, token_.revision + 1);

  geofence_format::Record record;
  record.generation = pending_generation_;
  record.token = pending_token_;
  record.snapshot = pending_snapshot_;
  if (!geofence_format::encode(record, blob_, sizeof(blob_))) return false;

  job_ = Job::kErase;
  blob_step_ = BlobStep::kBody;
  flash_op_awaiting_completion_ = false;
  return true;
}

FlashOpResult GeofenceStore::writeBlob() {
  const uint32_t offset = target_page_ * storage_config::kPageSize;
  constexpr size_t kBodyAndCrcSize = geofence_format::kCrcOffset + 4U;

  if (blob_step_ == BlobStep::kBody) {
    const FlashOpResult result =
        flash_op_awaiting_completion_
            ? flash_.pollPending()
            : flash_.program(offset, blob_, kBodyAndCrcSize);
    if (result == FlashOpResult::kPending) {
      flash_op_awaiting_completion_ = true;
      return FlashOpResult::kPending;
    }

    flash_op_awaiting_completion_ = false;
    if (result == FlashOpResult::kFailed) {
      failMutation();
      return FlashOpResult::kFailed;
    }

    if (!flash_.read(offset, scratch_, kBodyAndCrcSize) ||
        memcmp(scratch_, blob_, kBodyAndCrcSize) != 0) {
      failMutation();
      return FlashOpResult::kFailed;
    }
    blob_step_ = BlobStep::kCommit;
  }

  if (blob_step_ == BlobStep::kCommit) {
    const FlashOpResult result =
        flash_op_awaiting_completion_
            ? flash_.pollPending()
            : flash_.program(offset + geofence_format::kCommitOffset,
                             blob_ + geofence_format::kCommitOffset, 4);
    if (result == FlashOpResult::kPending) {
      flash_op_awaiting_completion_ = true;
      return FlashOpResult::kPending;
    }

    flash_op_awaiting_completion_ = false;
    if (result == FlashOpResult::kFailed) {
      failMutation();
      return FlashOpResult::kFailed;
    }
    blob_step_ = BlobStep::kVerify;
  }

  if (!flash_.read(offset, scratch_, sizeof(scratch_)) ||
      memcmp(scratch_, blob_, sizeof(scratch_)) != 0) {
    failMutation();
    return FlashOpResult::kFailed;
  }

  inspection_scratch_ = geofence_format::PageInspection{};
  if (!geofence_format::inspectPage(scratch_, sizeof(scratch_),
                                    inspection_scratch_) ||
      !geofence_format::isAuthoritativeCommitted(
          inspection_scratch_.evidence) ||
      !inspection_scratch_.has_decoded_record ||
      inspection_scratch_.record.generation != pending_generation_ ||
      inspection_scratch_.record.token.incarnation !=
          pending_token_.incarnation ||
      inspection_scratch_.record.token.revision != pending_token_.revision ||
      !sameSnapshot(inspection_scratch_.record.snapshot, pending_snapshot_)) {
    failMutation();
    return FlashOpResult::kFailed;
  }

  blob_step_ = BlobStep::kBody;
  return FlashOpResult::kDone;
}

void GeofenceStore::failMutation() {
  ++diagnostics_.mutation_failures;
  mutation_result_ready_ = true;
  mutation_success_ = false;
  job_ = Job::kNone;
  blob_step_ = BlobStep::kBody;
  flash_op_awaiting_completion_ = false;

  token_state_ = GeofenceTokenState::kUncertain;
  recovery_pending_ = true;

  if (flash_.hasUnreconciledMutation()) {
    mutation_unreconciled_ = true;
    ++diagnostics_.unreconciled_mutation_faults;
  }
}

void GeofenceStore::finishMutation() {
  active_page_ = static_cast<int>(target_page_);
  generation_ = pending_generation_;
  snapshot_ = pending_snapshot_;
  token_ = pending_token_;
  resource_state_ = resourceStateOf(snapshot_);
  token_state_ = GeofenceTokenState::kValid;
  semantic_unambiguous_ = true;
  maintenance_reset_required_ = false;
  job_ = Job::kNone;
  ++diagnostics_.mutations;
  mutation_result_ready_ = true;
  mutation_success_ = true;
}

void GeofenceStore::poll() {
  if (!ready_) return;

  if (mutation_unreconciled_) {
    if (flash_.hasUnreconciledMutation()) return;
    mutation_unreconciled_ = false;
    recovery_pending_ = true;
  }

  if (job_ == Job::kNone) {
    if (recovery_pending_) {
      recovery_pending_ = false;
      if (!recover()) {
        token_state_ = GeofenceTokenState::kUncertain;
        maintenance_reset_required_ = true;
        ready_ = false;
        return;
      }
      ++diagnostics_.recovery_reconciliations;
    }
    return;
  }

  if (job_ == Job::kErase) {
    const FlashOpResult result =
        flash_op_awaiting_completion_
            ? flash_.pollPending()
            : flash_.erasePage(target_page_);
    if (result == FlashOpResult::kPending) {
      flash_op_awaiting_completion_ = true;
      return;
    }

    flash_op_awaiting_completion_ = false;
    if (result == FlashOpResult::kFailed) {
      failMutation();
      return;
    }
    job_ = Job::kWrite;
    return;
  }

  if (writeBlob() != FlashOpResult::kDone) return;
  finishMutation();
}

bool GeofenceStore::takeMutationResult(bool& success) {
  if (!mutation_result_ready_) return false;
  success = mutation_success_;
  mutation_result_ready_ = false;
  return true;
}

}  // namespace orun_tlp
