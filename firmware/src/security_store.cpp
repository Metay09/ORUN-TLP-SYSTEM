#include "security_store.h"

#include <string.h>

#include "journal_format.h"
#include "storage_config.h"

namespace orun_tlp {
using namespace security_format;
namespace {
constexpr unsigned kPageCount = storage_config::kFutureSecurityRegionPages;
static_assert(kPageCount == 2, "A/B ping-pong assumes exactly two pages");

uint32_t pageOffset(unsigned page) {
  return page * storage_config::kPageSize;
}
uint32_t headerOffset(unsigned page) {
  return pageOffset(page) + pageHeaderOffset();
}
uint32_t credentialOffset(unsigned page) {
  return pageOffset(page) + credentialRecordOffset();
}
uint32_t v1ReserveOffset(unsigned page, unsigned slot) {
  return pageOffset(page) + txReserveRecordOffset(slot);
}
uint32_t v2StateOffset(unsigned page, unsigned slot) {
  return pageOffset(page) + securityStateRecordOffset(slot);
}
uint32_t v2TailOffset(unsigned page) {
  return pageOffset(page) + securityStateRecordOffset(kSecurityStateSlotsPerPage);
}
}  // namespace

bool SecurityStore::begin(DeviceIdentity device_identity) {
  device_identity_ = device_identity;
  ready_ = false;
  job_ = Job::kNone;
  new_page_purpose_ = NewPagePurpose::kCredentialCommit;
  phase_ = Phase::kErasePage;
  blob_step_ = BlobStep::kBody;
  flash_op_awaiting_completion_ = false;
  diagnostics_ = {};
  active_page_ = -1;
  newest_generation_ = 0;
  pages_[0] = pages_[1] = PageMeta{};
  credential_ = {};
  tx_reserved_bound_ = 0;
  tx_next_ = 0;
  a2d_replay_bound_ = 0;
  a2d_runtime_hwm_ = 0;
  a2d_runtime_hwm_valid_ = false;
  exhausted_ = false;
  migration_needed_ = false;
  migration_attempted_ = false;
  reserve_after_new_page_ = false;
  replay_after_new_page_ = false;
  pending_tx_bound_ = 0;
  pending_a2d_bound_ = 0;
  pending_replay_counter_ = 0;
  pending_replay_bound_ = 0;
  erase_old_page_ = -1;
  commit_result_ready_ = false;
  commit_success_ = false;
  a2d_result_ready_ = false;
  a2d_result_accepted_ = false;
  state_ = SecurityState::kFault;

  // critical_/maint_ are priority-tagged views of the same physical security
  // backend. begin() one view only, matching the existing M7P6B ownership.
  if (!critical_.begin()) return false;
  if (!recover()) return false;

  ready_ = true;
  if (state_ == SecurityState::kProvisioned) {
    if (migration_needed_) {
      migration_attempted_ = true;
      (void)startNewPage(NewPagePurpose::kMigration, credential_);
    } else {
      (void)startReservation();
    }
  }
  return true;
}

bool SecurityStore::recover() {
  struct PageClass {
    bool valid = false;
    uint8_t version = 0;
    uint64_t generation = 0;
    uint64_t device_identity = 0;
    Credential credential{};
  };

  PageClass classified[kPageCount]{};
  bool any_unsupported = false;
  bool any_committed_corruption = false;

  for (unsigned page = 0; page < kPageCount; ++page) {
    uint8_t header_bytes[kPageHeaderSize];
    if (!critical_.read(headerOffset(page), header_bytes,
                        sizeof(header_bytes)))
      return false;

    if (journal_format::erased(header_bytes, sizeof(header_bytes))) continue;

    uint8_t version = 0;
    if (!headerMagicPresent(header_bytes, &version)) {
      const bool activation_erased = journal_format::erased(
          header_bytes + kPageHeaderSize - sizeof(uint32_t),
          sizeof(uint32_t));
      ++diagnostics_.recovery_corruptions;
      if (!activation_erased) any_committed_corruption = true;
      continue;
    }

    if (version != kVersionV1 && version != kVersionV2) {
      any_unsupported = true;
      continue;
    }

    PageHeader header{};
    if (!decodePageHeaderVersion(header_bytes, version, header)) {
      if (journal_format::erased(
              header_bytes + kPageHeaderSize - sizeof(uint32_t),
              sizeof(uint32_t))) {
        // Interrupted page build: activation never landed, so an older
        // committed page may remain authoritative.
        ++diagnostics_.recovery_corruptions;
      } else {
        ++diagnostics_.recovery_corruptions;
        any_committed_corruption = true;
      }
      continue;
    }

    uint8_t cred_bytes[kCredentialRecordSize];
    if (!critical_.read(credentialOffset(page), cred_bytes,
                        sizeof(cred_bytes)))
      return false;
    Credential candidate{};
    if (!decodeCredential(cred_bytes, candidate) ||
        candidate.device_identity != header.device_identity) {
      ++diagnostics_.recovery_corruptions;
      any_committed_corruption = true;
      continue;
    }

    classified[page].valid = true;
    classified[page].version = version;
    classified[page].generation = header.generation;
    classified[page].device_identity = header.device_identity;
    classified[page].credential = candidate;
    pages_[page].generation = header.generation;
    pages_[page].version = version;
  }

  // A future recognized schema is a downgrade boundary. New firmware also
  // keeps the M7P6B rule that ambiguous committed state blocks fallback.
  if (any_unsupported) {
    state_ = SecurityState::kUnsupported;
    active_page_ = -1;
    newest_generation_ = 0;
    return true;
  }
  if (any_committed_corruption) {
    state_ = SecurityState::kFault;
    active_page_ = -1;
    newest_generation_ = 0;
    return true;
  }

  int winner = -1;
  uint64_t best_generation = 0;
  for (unsigned page = 0; page < kPageCount; ++page) {
    if (!classified[page].valid) continue;
    if (classified[page].generation > best_generation) {
      best_generation = classified[page].generation;
      winner = static_cast<int>(page);
    } else if (classified[page].generation == best_generation &&
               best_generation != 0) {
      // A legitimate A/B transaction always advances generation. Two
      // committed pages with the same highest generation are ambiguous
      // authority and could carry different security bounds; never pick one
      // by page index and risk rollback.
      ++diagnostics_.recovery_corruptions;
      state_ = SecurityState::kFault;
      active_page_ = -1;
      newest_generation_ = 0;
      return true;
    }
  }

  if (winner < 0) {
    state_ = SecurityState::kUnprovisioned;
    active_page_ = -1;
    newest_generation_ = 0;
    return true;
  }

  active_page_ = winner;
  newest_generation_ = best_generation;
  if (classified[winner].device_identity !=
      device_identity_.legacyUint64()) {
    state_ = SecurityState::kForeign;
    return true;
  }

  credential_ = classified[winner].credential;
  state_ = SecurityState::kProvisioned;
  tx_reserved_bound_ = 0;
  a2d_replay_bound_ = 0;

  if (classified[winner].version == kVersionV1) {
    for (unsigned slot = 0; slot < kV1TxReserveSlotsPerPage; ++slot) {
      uint8_t bytes[kTxReserveRecordSize];
      if (!critical_.read(v1ReserveOffset(winner, slot), bytes,
                          sizeof(bytes)))
        return false;
      if (journal_format::erased(bytes, sizeof(bytes))) {
        for (unsigned later = slot + 1; later < kV1TxReserveSlotsPerPage;
             ++later) {
          uint8_t later_bytes[kTxReserveRecordSize];
          if (!critical_.read(v1ReserveOffset(winner, later), later_bytes,
                              sizeof(later_bytes)))
            return false;
          if (!journal_format::erased(later_bytes, sizeof(later_bytes))) {
            ++diagnostics_.recovery_corruptions;
            state_ = SecurityState::kFault;
            return true;
          }
        }
        break;
      }

      pages_[winner].state_used = slot + 1;
      TxReserve reserve{};
      if (!decodeTxReserve(bytes, reserve) ||
          !credentialIdEqual(reserve.credential_id,
                             credential_.credential_id) ||
          reserve.key_epoch != credential_.key_epoch) {
        ++diagnostics_.recovery_corruptions;
        state_ = SecurityState::kFault;
        return true;
      }
      if (reserve.tx_reserved_bound > tx_reserved_bound_)
        tx_reserved_bound_ = reserve.tx_reserved_bound;
    }

    // A valid, device-bound v1 page is the only state eligible for automatic
    // migration. FOREIGN/UNSUPPORTED/FAULT returned above and never get here.
    migration_needed_ = true;
  } else {
    for (unsigned slot = 0; slot < kSecurityStateSlotsPerPage; ++slot) {
      uint8_t bytes[kSecurityStateRecordSize];
      if (!critical_.read(v2StateOffset(winner, slot), bytes,
                          sizeof(bytes)))
        return false;
      if (journal_format::erased(bytes, sizeof(bytes))) {
        for (unsigned later = slot + 1; later < kSecurityStateSlotsPerPage;
             ++later) {
          uint8_t later_bytes[kSecurityStateRecordSize];
          if (!critical_.read(v2StateOffset(winner, later), later_bytes,
                              sizeof(later_bytes)))
            return false;
          if (!journal_format::erased(later_bytes, sizeof(later_bytes))) {
            ++diagnostics_.recovery_corruptions;
            state_ = SecurityState::kFault;
            return true;
          }
        }
        break;
      }

      pages_[winner].state_used = slot + 1;
      SecurityStateRecord record{};
      if (!decodeSecurityState(bytes, record) ||
          !credentialIdEqual(record.credential_id,
                             credential_.credential_id) ||
          record.key_epoch != credential_.key_epoch) {
        ++diagnostics_.recovery_corruptions;
        state_ = SecurityState::kFault;
        return true;
      }

      if (record.kind == SecurityStateKind::kTxReserveExclusiveBound) {
        if (record.value < tx_reserved_bound_) {
          ++diagnostics_.recovery_corruptions;
          state_ = SecurityState::kFault;
          return true;
        }
        tx_reserved_bound_ = record.value;
      } else if (
          record.kind == SecurityStateKind::kA2dReplayExclusiveBound) {
        if (record.value < a2d_replay_bound_) {
          ++diagnostics_.recovery_corruptions;
          state_ = SecurityState::kFault;
          return true;
        }
        a2d_replay_bound_ = record.value;
      } else {
        ++diagnostics_.recovery_corruptions;
        state_ = SecurityState::kFault;
        return true;
      }
    }

    // v2 deliberately reserves the 36-byte page tail. Any programmed byte
    // there is an unknown state/schema extension and therefore fail-closed.
    uint8_t tail[kSecurityStateTailBytes];
    if (!critical_.read(v2TailOffset(winner), tail, sizeof(tail)))
      return false;
    if (!journal_format::erased(tail, sizeof(tail))) {
      ++diagnostics_.recovery_corruptions;
      state_ = SecurityState::kFault;
      return true;
    }
  }

  // Reboot never reuses unused TX counters. For replay, every counter below
  // the durable exclusive bound is conservatively burned across reset.
  tx_next_ = tx_reserved_bound_;
  if (a2d_replay_bound_ != 0) {
    a2d_runtime_hwm_ = a2d_replay_bound_ - 1;
    a2d_runtime_hwm_valid_ = true;
  } else {
    a2d_runtime_hwm_ = 0;
    a2d_runtime_hwm_valid_ = false;
  }
  return true;
}

bool SecurityStore::currentCredentialId(
    uint8_t (&out)[kCredentialIdSize]) const {
  if (state_ != SecurityState::kProvisioned) return false;
  memcpy(out, credential_.credential_id, kCredentialIdSize);
  return true;
}

bool SecurityStore::commitCredential(
    const uint8_t (&credential_id)[kCredentialIdSize], uint32_t key_epoch,
    const uint8_t (&k_root)[kKRootSize]) {
  if (!ready_ || busy()) return false;
  if (state_ != SecurityState::kUnprovisioned &&
      state_ != SecurityState::kProvisioned)
    return false;
  // Result ownership crosses credential lifetimes: do not let a credential
  // replacement silently invalidate an unread replay decision from the
  // current credential, just as an unread prior commit result blocks another
  // credential commit.
  if (commit_result_ready_ || a2d_result_ready_) return false;

  Credential candidate{};
  memcpy(candidate.credential_id, credential_id, kCredentialIdSize);
  candidate.key_epoch = key_epoch;
  candidate.device_identity = device_identity_.legacyUint64();
  memcpy(candidate.k_root, k_root, kKRootSize);

  if (state_ == SecurityState::kProvisioned &&
      (credentialIdEqual(candidate.credential_id,
                         credential_.credential_id) ||
       memcmp(candidate.k_root, credential_.k_root, kKRootSize) == 0)) {
    return false;
  }

  reserve_after_new_page_ = false;
  return startNewPage(NewPagePurpose::kCredentialCommit, candidate);
}

bool SecurityStore::takeCommitResult(bool& success) {
  if (!commit_result_ready_) return false;
  success = commit_success_;
  commit_result_ready_ = false;
  return true;
}

bool SecurityStore::reserveNextTxCounter(uint64_t& counter,
                                         uint32_t& key_epoch) {
  if (!ready_ || busy() || state_ != SecurityState::kProvisioned ||
      exhausted_)
    return false;
  if (tx_next_ >= tx_reserved_bound_) return false;
  counter = tx_next_++;
  key_epoch = credential_.key_epoch;
  ++diagnostics_.tx_counters_issued;
  return true;
}

bool SecurityStore::submitAuthenticatedA2dCounter(uint64_t counter) {
  if (!ready_ || busy() || state_ != SecurityState::kProvisioned ||
      a2d_result_ready_)
    return false;

  if (a2d_runtime_hwm_valid_ && counter <= a2d_runtime_hwm_) {
    a2d_result_ready_ = true;
    a2d_result_accepted_ = false;
    ++diagnostics_.a2d_rejections;
    return true;
  }

  if (counter < a2d_replay_bound_) {
    a2d_runtime_hwm_ = counter;
    a2d_runtime_hwm_valid_ = true;
    a2d_result_ready_ = true;
    a2d_result_accepted_ = true;
    ++diagnostics_.a2d_admissions;
    return true;
  }

  const uint64_t max_bound =
      (UINT64_MAX / kA2dReplayReservationBlockSize) *
      kA2dReplayReservationBlockSize;
  if (counter >= max_bound) {
    a2d_result_ready_ = true;
    a2d_result_accepted_ = false;
    ++diagnostics_.a2d_rejections;
    ++diagnostics_.a2d_exhausted_events;
    return true;
  }

  const uint64_t bound =
      (counter / kA2dReplayReservationBlockSize + 1) *
      kA2dReplayReservationBlockSize;
  return startA2dReplayReservation(counter, bound);
}

bool SecurityStore::takeA2dReplayResult(bool& accepted) {
  if (!a2d_result_ready_) return false;
  accepted = a2d_result_accepted_;
  a2d_result_ready_ = false;
  return true;
}

bool SecurityStore::startNewPage(NewPagePurpose purpose,
                                 const Credential& credential) {
  if (!ready_ || busy()) return false;

  target_page_ =
      active_page_ < 0 ? 0 : static_cast<uint32_t>(1 - active_page_);
  target_generation_ = newest_generation_ + 1;
  if (target_generation_ == 0) return false;

  erase_old_page_ =
      (active_page_ >= 0 && pages_[active_page_].generation != 0)
          ? active_page_
          : -1;
  pending_credential_ = credential;
  pending_tx_bound_ =
      purpose == NewPagePurpose::kCredentialCommit ? 0 : tx_reserved_bound_;
  pending_a2d_bound_ =
      purpose == NewPagePurpose::kCredentialCommit ? 0 : a2d_replay_bound_;
  new_page_purpose_ = purpose;

  // Erase is maintenance. A brand-new credential snapshot becomes critical
  // only after the destination page is erased; migration/compaction stay
  // maintenance work.
  active_port_ = &maint_;
  job_ = Job::kNewPage;
  phase_ = Phase::kErasePage;
  blob_step_ = BlobStep::kBody;
  flash_op_awaiting_completion_ = false;
  return true;
}

bool SecurityStore::startEraseOld() {
  active_port_ = &maint_;
  target_page_ = static_cast<uint32_t>(erase_old_page_);
  job_ = Job::kEraseOld;
  phase_ = Phase::kEraseOldPage;
  flash_op_awaiting_completion_ = false;
  return true;
}

bool SecurityStore::startReservation() {
  if (!ready_ || busy() || state_ != SecurityState::kProvisioned ||
      active_page_ < 0)
    return false;

  if (tx_reserved_bound_ > UINT64_MAX - kTxReservationBlockSize) {
    if (!exhausted_) ++diagnostics_.exhausted_events;
    exhausted_ = true;
    return false;
  }

  const uint8_t version = pages_[active_page_].version;
  if (version == kVersionV1) {
    // v1 is read/migration-only in this firmware. Never extend the legacy log
    // after booting code that understands v2. If the automatic migration
    // failed earlier this boot, protected TX remains unavailable rather than
    // silently creating fresh v1 state.
    if (migration_attempted_) return false;
    migration_attempted_ = true;
    reserve_after_new_page_ = true;
    if (!startNewPage(NewPagePurpose::kMigration, credential_)) {
      reserve_after_new_page_ = false;
      return false;
    }
    return true;
  }
  if (version != kVersionV2) return false;

  if (pages_[active_page_].state_used + 1 >=
      kSecurityStateSlotsPerPage) {
    reserve_after_new_page_ = true;
    return startNewPage(NewPagePurpose::kCompaction, credential_);
  }

  pending_tx_bound_ = tx_reserved_bound_ + kTxReservationBlockSize;
  active_port_ = &critical_;
  target_page_ = static_cast<uint32_t>(active_page_);
  target_slot_ = pages_[active_page_].state_used;
  job_ = Job::kReserve;
  phase_ = Phase::kWriteReserve;

  SecurityStateRecord state{};
  memcpy(state.credential_id, credential_.credential_id,
         kCredentialIdSize);
  state.key_epoch = credential_.key_epoch;
  state.kind = SecurityStateKind::kTxReserveExclusiveBound;
  state.value = pending_tx_bound_;
  uint8_t bytes[kSecurityStateRecordSize];
  encodeSecurityState(state, bytes);
  startBlob(v2StateOffset(target_page_, target_slot_), bytes,
            sizeof(bytes));
  return true;
}

bool SecurityStore::startA2dReplayReservation(uint64_t counter,
                                                uint64_t bound) {
  if (!ready_ || busy() || state_ != SecurityState::kProvisioned ||
      active_page_ < 0 || a2d_result_ready_)
    return false;

  pending_replay_counter_ = counter;
  pending_replay_bound_ = bound;

  const uint8_t version = pages_[active_page_].version;
  if (version == kVersionV1) {
    replay_after_new_page_ = true;
    migration_attempted_ = true;
    if (!startNewPage(NewPagePurpose::kMigration, credential_)) {
      replay_after_new_page_ = false;
      return false;
    }
    return true;
  }
  if (version != kVersionV2) return false;

  if (pages_[active_page_].state_used + 1 >=
      kSecurityStateSlotsPerPage) {
    replay_after_new_page_ = true;
    if (!startNewPage(NewPagePurpose::kCompaction, credential_)) {
      replay_after_new_page_ = false;
      return false;
    }
    return true;
  }

  SecurityStateRecord state{};
  memcpy(state.credential_id, credential_.credential_id,
         kCredentialIdSize);
  state.key_epoch = credential_.key_epoch;
  state.kind = SecurityStateKind::kA2dReplayExclusiveBound;
  state.value = bound;
  uint8_t bytes[kSecurityStateRecordSize];
  encodeSecurityState(state, bytes);

  active_port_ = &critical_;
  target_page_ = static_cast<uint32_t>(active_page_);
  target_slot_ = pages_[active_page_].state_used;
  job_ = Job::kA2dReplayReserve;
  phase_ = Phase::kWriteReserve;
  startBlob(v2StateOffset(target_page_, target_slot_), bytes,
            sizeof(bytes));
  return true;
}

void SecurityStore::startBlob(uint32_t offset, const uint8_t* bytes,
                              uint32_t size) {
  memcpy(blob_, bytes, size);
  blob_offset_ = offset;
  blob_size_ = size;
  blob_step_ = BlobStep::kBody;
  flash_op_awaiting_completion_ = false;
}

void SecurityStore::startSnapshotTxState() {
  SecurityStateRecord state{};
  memcpy(state.credential_id, pending_credential_.credential_id,
         kCredentialIdSize);
  state.key_epoch = pending_credential_.key_epoch;
  state.kind = SecurityStateKind::kTxReserveExclusiveBound;
  state.value = pending_tx_bound_;
  uint8_t bytes[kSecurityStateRecordSize];
  encodeSecurityState(state, bytes);
  phase_ = Phase::kWriteTxState;
  startBlob(v2StateOffset(target_page_, target_slot_), bytes, sizeof(bytes));
}

void SecurityStore::startSnapshotA2dState() {
  SecurityStateRecord state{};
  memcpy(state.credential_id, pending_credential_.credential_id,
         kCredentialIdSize);
  state.key_epoch = pending_credential_.key_epoch;
  state.kind = SecurityStateKind::kA2dReplayExclusiveBound;
  state.value = pending_a2d_bound_;
  uint8_t bytes[kSecurityStateRecordSize];
  encodeSecurityState(state, bytes);
  phase_ = Phase::kWriteA2dState;
  startBlob(v2StateOffset(target_page_, target_slot_), bytes, sizeof(bytes));
}

void SecurityStore::startSnapshotCredential() {
  uint8_t bytes[kCredentialRecordSize];
  encodeCredential(pending_credential_, bytes);
  phase_ = Phase::kWriteCredential;
  startBlob(credentialOffset(target_page_), bytes, sizeof(bytes));
}

FlashOpResult SecurityStore::writeBlob() {
  FlashBackend& port = *active_port_;

  if (blob_step_ == BlobStep::kBody) {
    const FlashOpResult result =
        flash_op_awaiting_completion_
            ? port.pollPending()
            : port.program(blob_offset_, blob_, blob_size_ - 4);
    if (result == FlashOpResult::kPending) {
      flash_op_awaiting_completion_ = true;
      return FlashOpResult::kPending;
    }
    flash_op_awaiting_completion_ = false;
    if (result == FlashOpResult::kFailed) {
      fail();
      return FlashOpResult::kFailed;
    }
    blob_step_ = BlobStep::kCommit;
  }

  if (blob_step_ == BlobStep::kCommit) {
    const FlashOpResult result =
        flash_op_awaiting_completion_
            ? port.pollPending()
            : port.program(blob_offset_ + blob_size_ - 4,
                           blob_ + blob_size_ - 4, 4);
    if (result == FlashOpResult::kPending) {
      flash_op_awaiting_completion_ = true;
      return FlashOpResult::kPending;
    }
    flash_op_awaiting_completion_ = false;
    if (result == FlashOpResult::kFailed) {
      fail();
      return FlashOpResult::kFailed;
    }
    blob_step_ = BlobStep::kVerify;
  }

  uint8_t verify[kCredentialRecordSize];
  if (!port.read(blob_offset_, verify, blob_size_) ||
      memcmp(verify, blob_, blob_size_) != 0) {
    blob_step_ = BlobStep::kBody;
    fail();
    return FlashOpResult::kFailed;
  }

  blob_step_ = BlobStep::kBody;
  return FlashOpResult::kDone;
}

FlashOpResult SecurityStore::writeBlobBodyOnly() {
  FlashBackend& port = *active_port_;
  const uint32_t body_size = blob_size_ - sizeof(uint32_t);
  const FlashOpResult result =
      flash_op_awaiting_completion_
          ? port.pollPending()
          : port.program(blob_offset_, blob_, body_size);
  if (result == FlashOpResult::kPending) {
    flash_op_awaiting_completion_ = true;
    return FlashOpResult::kPending;
  }
  flash_op_awaiting_completion_ = false;
  if (result == FlashOpResult::kFailed) {
    fail();
    return FlashOpResult::kFailed;
  }

  uint8_t verify[kCredentialRecordSize];
  if (!port.read(blob_offset_, verify, body_size) ||
      memcmp(verify, blob_, body_size) != 0) {
    fail();
    return FlashOpResult::kFailed;
  }
  return FlashOpResult::kDone;
}

FlashOpResult SecurityStore::writePageActivation() {
  FlashBackend& port = *active_port_;
  memset(blob_, 0, sizeof(uint32_t));
  const uint32_t offset =
      headerOffset(target_page_) + kPageHeaderSize - sizeof(uint32_t);
  const FlashOpResult result =
      flash_op_awaiting_completion_
          ? port.pollPending()
          : port.program(offset, blob_, sizeof(uint32_t));
  if (result == FlashOpResult::kPending) {
    flash_op_awaiting_completion_ = true;
    return FlashOpResult::kPending;
  }
  flash_op_awaiting_completion_ = false;
  if (result == FlashOpResult::kFailed) {
    fail();
    return FlashOpResult::kFailed;
  }

  uint8_t verify[kPageHeaderSize];
  if (!port.read(headerOffset(target_page_), verify, sizeof(verify))) {
    fail();
    return FlashOpResult::kFailed;
  }
  PageHeader decoded{};
  if (!decodePageHeader(verify, decoded) ||
      decoded.generation != target_generation_ ||
      decoded.device_identity != device_identity_.legacyUint64()) {
    fail();
    return FlashOpResult::kFailed;
  }
  return FlashOpResult::kDone;
}

void SecurityStore::fail() {
  const Job failing_job = job_;
  const NewPagePurpose failing_purpose = new_page_purpose_;
  const bool was_reserve_after_new_page = reserve_after_new_page_;
  const bool was_replay_after_new_page = replay_after_new_page_;

  job_ = Job::kNone;
  phase_ = Phase::kErasePage;
  blob_step_ = BlobStep::kBody;
  flash_op_awaiting_completion_ = false;
  reserve_after_new_page_ = false;
  replay_after_new_page_ = false;

  // Existing active page/state remains authoritative because every new-page
  // failure happens before activation or every append failure targets only the
  // next erased slot.
  if (failing_job == Job::kReserve) {
    ++diagnostics_.reservation_failures;
  } else if (failing_job == Job::kA2dReplayReserve) {
    ++diagnostics_.a2d_reservation_failures;
    a2d_result_ready_ = true;
    a2d_result_accepted_ = false;
  } else if (failing_job == Job::kNewPage) {
    if (failing_purpose == NewPagePurpose::kCredentialCommit) {
      commit_result_ready_ = true;
      commit_success_ = false;
      ++diagnostics_.commit_failures;
    } else if (failing_purpose == NewPagePurpose::kMigration) {
      ++diagnostics_.migration_failures;
      if (was_reserve_after_new_page)
        ++diagnostics_.reservation_failures;
      if (was_replay_after_new_page) {
        ++diagnostics_.a2d_reservation_failures;
        a2d_result_ready_ = true;
        a2d_result_accepted_ = false;
      }
    } else {
      if (was_replay_after_new_page) {
        ++diagnostics_.a2d_reservation_failures;
        a2d_result_ready_ = true;
        a2d_result_accepted_ = false;
      } else {
        ++diagnostics_.reservation_failures;
      }
    }
  }
}

void SecurityStore::completeNewPage() {
  pages_[target_page_].generation = target_generation_;
  pages_[target_page_].version = kVersionV2;
  pages_[target_page_].state_used =
      (pending_tx_bound_ != 0 ? 1U : 0U) +
      (pending_a2d_bound_ != 0 ? 1U : 0U);

  active_page_ = static_cast<int>(target_page_);
  newest_generation_ = target_generation_;
  credential_ = pending_credential_;
  state_ = SecurityState::kProvisioned;
  job_ = Job::kNone;

  if (new_page_purpose_ == NewPagePurpose::kCredentialCommit) {
    tx_reserved_bound_ = 0;
    tx_next_ = 0;
    a2d_replay_bound_ = 0;
    a2d_runtime_hwm_ = 0;
    a2d_runtime_hwm_valid_ = false;
    a2d_result_ready_ = false;
    a2d_result_accepted_ = false;
    exhausted_ = false;
    migration_needed_ = false;
    migration_attempted_ = false;
    ++diagnostics_.commits;
    commit_result_ready_ = true;
    commit_success_ = true;
  } else if (new_page_purpose_ == NewPagePurpose::kMigration) {
    migration_needed_ = false;
    migration_attempted_ = false;
    ++diagnostics_.migrations;
  } else {
    ++diagnostics_.compactions;
  }

  if (erase_old_page_ >= 0) {
    startEraseOld();
    return;
  }
  if (reserve_after_new_page_) {
    reserve_after_new_page_ = false;
    startReservation();
    return;
  }
  if (replay_after_new_page_) {
    replay_after_new_page_ = false;
    startA2dReplayReservation(pending_replay_counter_,
                              pending_replay_bound_);
    return;
  }
  maybeAutoReserve();
}

void SecurityStore::completeEraseOld(bool success) {
  if (!success) {
    ++diagnostics_.old_page_erase_failures;
  } else {
    pages_[static_cast<unsigned>(erase_old_page_)] = PageMeta{};
  }
  erase_old_page_ = -1;
  job_ = Job::kNone;

  if (reserve_after_new_page_) {
    reserve_after_new_page_ = false;
    startReservation();
    return;
  }
  if (replay_after_new_page_) {
    replay_after_new_page_ = false;
    startA2dReplayReservation(pending_replay_counter_,
                              pending_replay_bound_);
    return;
  }
  maybeAutoReserve();
}

void SecurityStore::completeReserve() {
  pages_[target_page_].state_used = target_slot_ + 1;
  tx_reserved_bound_ = pending_tx_bound_;
  job_ = Job::kNone;
  ++diagnostics_.reservations;
}

void SecurityStore::completeA2dReplayReserve() {
  pages_[target_page_].state_used = target_slot_ + 1;
  a2d_replay_bound_ = pending_replay_bound_;
  a2d_runtime_hwm_ = pending_replay_counter_;
  a2d_runtime_hwm_valid_ = true;
  job_ = Job::kNone;
  a2d_result_ready_ = true;
  a2d_result_accepted_ = true;
  ++diagnostics_.a2d_reservations;
  ++diagnostics_.a2d_admissions;
}

void SecurityStore::maybeAutoReserve() {
  if (job_ != Job::kNone || state_ != SecurityState::kProvisioned ||
      exhausted_)
    return;

  if (migration_needed_) {
    if (!migration_attempted_) {
      migration_attempted_ = true;
      (void)startNewPage(NewPagePurpose::kMigration, credential_);
    }
    // Never fall through to a legacy v1 TX append when migration is still
    // required or failed this boot.
    return;
  }

  if (tx_next_ == tx_reserved_bound_) (void)startReservation();
}

void SecurityStore::poll() {
  if (!ready_) return;

  if (job_ == Job::kNone) {
    maybeAutoReserve();
    return;
  }

  if (phase_ == Phase::kErasePage) {
    const FlashOpResult result =
        flash_op_awaiting_completion_
            ? active_port_->pollPending()
            : active_port_->erasePage(target_page_);
    if (result == FlashOpResult::kPending) {
      flash_op_awaiting_completion_ = true;
      return;
    }
    flash_op_awaiting_completion_ = false;
    if (result == FlashOpResult::kFailed) {
      fail();
      return;
    }

    active_port_ =
        new_page_purpose_ == NewPagePurpose::kCredentialCommit
            ? &critical_
            : &maint_;
    phase_ = Phase::kWriteHeader;
    PageHeader header{target_generation_,
                      device_identity_.legacyUint64()};
    uint8_t bytes[kPageHeaderSize];
    encodePageHeader(header, bytes);
    startBlob(headerOffset(target_page_), bytes, sizeof(bytes));
    return;
  }

  if (phase_ == Phase::kEraseOldPage) {
    const FlashOpResult result =
        flash_op_awaiting_completion_
            ? active_port_->pollPending()
            : active_port_->erasePage(target_page_);
    if (result == FlashOpResult::kPending) {
      flash_op_awaiting_completion_ = true;
      return;
    }
    flash_op_awaiting_completion_ = false;
    completeEraseOld(result == FlashOpResult::kDone);
    return;
  }

  if (phase_ == Phase::kWriteHeader) {
    // Header activation is intentionally left erased until the complete v2
    // snapshot (TX bound, A2D bound, credential as applicable) is durable.
    if (writeBlobBodyOnly() != FlashOpResult::kDone) return;
    target_slot_ = 0;
    if (pending_tx_bound_ != 0) {
      startSnapshotTxState();
      return;
    }
    if (pending_a2d_bound_ != 0) {
      startSnapshotA2dState();
      return;
    }
    startSnapshotCredential();
    return;
  }

  if (phase_ == Phase::kActivatePage) {
    if (writePageActivation() != FlashOpResult::kDone) return;
    completeNewPage();
    return;
  }

  if (writeBlob() != FlashOpResult::kDone) return;

  if (phase_ == Phase::kWriteTxState) {
    ++target_slot_;
    if (pending_a2d_bound_ != 0) {
      startSnapshotA2dState();
    } else {
      startSnapshotCredential();
    }
    return;
  }

  if (phase_ == Phase::kWriteA2dState) {
    ++target_slot_;
    startSnapshotCredential();
    return;
  }

  if (phase_ == Phase::kWriteCredential) {
    phase_ = Phase::kActivatePage;
    flash_op_awaiting_completion_ = false;
    return;
  }

  if (phase_ == Phase::kWriteReserve) {
    if (job_ == Job::kA2dReplayReserve) {
      completeA2dReplayReserve();
    } else {
      completeReserve();
    }
    return;
  }
}

}  // namespace orun_tlp
