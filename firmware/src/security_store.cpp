#include "security_store.h"

#include <string.h>

#include "journal_format.h"
#include "storage_config.h"

namespace orun_tlp {
using namespace security_format;
namespace {
constexpr unsigned kPageCount = storage_config::kFutureSecurityRegionPages;
static_assert(kPageCount == 2, "A/B ping-pong assumes exactly two pages");
uint32_t pageOffset(unsigned page) { return page * storage_config::kPageSize; }
uint32_t headerOffset(unsigned page) { return pageOffset(page) + pageHeaderOffset(); }
uint32_t credentialOffset(unsigned page) { return pageOffset(page) + credentialRecordOffset(); }
uint32_t reserveOffset(unsigned page, unsigned slot) {
  return pageOffset(page) + txReserveRecordOffset(slot);
}
}  // namespace

bool SecurityStore::begin(DeviceIdentity device_identity) {
  device_identity_ = device_identity;
  ready_ = false;
  job_ = Job::kNone;
  phase_ = Phase::kErasePage;
  blob_step_ = BlobStep::kBody;
  flash_op_awaiting_completion_ = false;
  diagnostics_ = {};
  active_page_ = -1;
  newest_generation_ = 0;
  pages_[0] = pages_[1] = PageMeta{};
  credential_ = {};
  tx_reserved_bound_ = tx_next_ = 0;
  exhausted_ = false;
  commit_result_ready_ = commit_success_ = false;
  reserve_after_new_page_ = false;
  erase_old_page_ = -1;
  state_ = SecurityState::kFault;
  // critical_/maint_ are two priority-tagged VIEWS of the same underlying
  // security backend (see the class comment) -- begin()ing one is
  // sufficient and avoids redundantly re-running the other's readiness
  // checks (register reads, SoftDevice state) for no new information.
  if (!critical_.begin()) return false;
  if (!recover()) return false;
  ready_ = true;
  if (state_ == SecurityState::kProvisioned) startReservation();
  return true;
}

bool SecurityStore::recover() {
  struct PageClass {
    bool valid = false;
    uint64_t generation = 0;
    uint64_t device_identity = 0;
    Credential credential{};
  };
  PageClass classified[kPageCount]{};
  bool any_unsupported = false;

  for (unsigned page = 0; page < kPageCount; ++page) {
    uint8_t header_bytes[kPageHeaderSize];
    if (!critical_.read(headerOffset(page), header_bytes, sizeof(header_bytes))) return false;
    uint8_t version = 0;
    if (!headerMagicPresent(header_bytes, &version)) continue;  // blank page.
    PageHeader header{};
    if (!decodePageHeader(header_bytes, header)) {
      if (version != kVersion) any_unsupported = true;
      else ++diagnostics_.recovery_corruptions;
      continue;
    }
    pages_[page].generation = header.generation;
    uint8_t cred_bytes[kCredentialRecordSize];
    if (!critical_.read(credentialOffset(page), cred_bytes, sizeof(cred_bytes))) return false;
    Credential candidate{};
    if (!decodeCredential(cred_bytes, candidate) ||
        candidate.device_identity != header.device_identity) {
      ++diagnostics_.recovery_corruptions;
      continue;
    }
    classified[page].valid = true;
    classified[page].generation = header.generation;
    classified[page].device_identity = header.device_identity;
    classified[page].credential = candidate;
  }

  // A recognized-but-unsupported security page is a downgrade boundary,
  // not ordinary corruption. Even if the other page is a valid v1 page, an
  // older firmware cannot know whether the unsupported page advanced the
  // credential/counter state. Using the older page could therefore roll
  // nonce state backward. Fail closed for the whole store.
  if (any_unsupported) {
    state_ = SecurityState::kUnsupported;
    active_page_ = -1;
    newest_generation_ = 0;
    return true;
  }

  int winner = -1;
  uint64_t best_generation = 0;
  for (unsigned page = 0; page < kPageCount; ++page) {
    if (classified[page].valid && classified[page].generation > best_generation) {
      best_generation = classified[page].generation;
      winner = static_cast<int>(page);
    }
  }

  if (winner < 0) {
    state_ = any_unsupported ? SecurityState::kUnsupported : SecurityState::kUnprovisioned;
    active_page_ = -1;
    newest_generation_ = 0;
    return true;
  }

  active_page_ = winner;
  newest_generation_ = best_generation;
  if (classified[winner].device_identity != device_identity_.legacyUint64()) {
    state_ = SecurityState::kForeign;
    return true;
  }

  credential_ = classified[winner].credential;
  state_ = SecurityState::kProvisioned;
  tx_reserved_bound_ = 0;
  for (unsigned slot = 0; slot < kTxReserveSlotsPerPage; ++slot) {
    uint8_t bytes[kTxReserveRecordSize];
    if (!critical_.read(reserveOffset(winner, slot), bytes, sizeof(bytes))) return false;
    if (journal_format::erased(bytes, sizeof(bytes))) break;
    pages_[winner].tx_reserve_used = slot + 1;
    TxReserve reserve{};
    if (!decodeTxReserve(bytes, reserve) ||
        !credentialIdEqual(reserve.credential_id, credential_.credential_id) ||
        reserve.key_epoch != credential_.key_epoch) {
      // This is the authoritative credential page. Skipping a non-erased
      // but invalid reservation and continuing from an earlier/lower bound
      // could reissue counters that had already been durably reserved and
      // used before the corruption. Security durability fails closed here:
      // keep legacy TLP v1 alive at the composition root, but never expose
      // protected TX counters from ambiguous security state.
      ++diagnostics_.recovery_corruptions;
      state_ = SecurityState::kFault;
      return true;
    }
    if (reserve.tx_reserved_bound > tx_reserved_bound_) tx_reserved_bound_ = reserve.tx_reserved_bound;
  }
  // A reboot never reuses unused counters: skip straight to the last durable
  // bound (discarding any headroom that remained in that block), then
  // begin()'s caller forces a fresh durable reservation before this store
  // exposes any counter to reserveNextTxCounter().
  tx_next_ = tx_reserved_bound_;
  return true;
}

bool SecurityStore::currentCredentialId(uint8_t (&out)[kCredentialIdSize]) const {
  if (state_ != SecurityState::kProvisioned) return false;
  memcpy(out, credential_.credential_id, kCredentialIdSize);
  return true;
}

bool SecurityStore::commitCredential(const uint8_t (&credential_id)[kCredentialIdSize],
                                     uint32_t key_epoch, const uint8_t (&k_root)[kKRootSize]) {
  if (!ready_ || busy()) return false;
  if (state_ != SecurityState::kUnprovisioned && state_ != SecurityState::kProvisioned) return false;
  if (commit_result_ready_) return false;
  Credential candidate{};
  memcpy(candidate.credential_id, credential_id, kCredentialIdSize);
  candidate.key_epoch = key_epoch;
  candidate.device_identity = device_identity_.legacyUint64();
  memcpy(candidate.k_root, k_root, kKRootSize);
  if (state_ == SecurityState::kProvisioned &&
      (credentialIdEqual(candidate.credential_id, credential_.credential_id) ||
       memcmp(candidate.k_root, credential_.k_root, kKRootSize) == 0)) {
    // Re-provisioning is a new security lifetime. Reusing the current
    // credential_id or current root while resetting the TX counter to zero
    // would make nonce/key reuse possible. Historical-root reuse remains a
    // provisioning-layer responsibility because this store intentionally
    // retains only the current credential.
    return false;
  }
  reserve_after_new_page_ = false;
  return startNewPage(/*critical=*/true, /*seed_reserve=*/false, candidate);
}

bool SecurityStore::takeCommitResult(bool& success) {
  if (!commit_result_ready_) return false;
  success = commit_success_;
  commit_result_ready_ = false;
  return true;
}

bool SecurityStore::reserveNextTxCounter(uint64_t& counter, uint32_t& key_epoch) {
  if (!ready_ || busy() || state_ != SecurityState::kProvisioned || exhausted_) return false;
  if (tx_next_ >= tx_reserved_bound_) return false;  // durable bound not yet available.
  counter = tx_next_++;
  key_epoch = credential_.key_epoch;
  ++diagnostics_.tx_counters_issued;
  return true;
}

bool SecurityStore::startNewPage(bool critical, bool seed_reserve, const Credential& credential) {
  if (!ready_ || busy()) return false;
  target_page_ = active_page_ < 0 ? 0 : static_cast<uint32_t>(1 - active_page_);
  target_generation_ = newest_generation_ + 1;
  if (target_generation_ == 0) return false;  // generation exhaustion; refuse rather than wrap.
  erase_old_page_ =
      (active_page_ >= 0 && pages_[active_page_].generation != 0) ? active_page_ : -1;
  pending_credential_ = credential;
  seed_reserve_ = seed_reserve;
  pending_tx_bound_ = tx_reserved_bound_;  // carried forward as-is for compaction only.
  active_port_ = critical ? &critical_ : &maint_;
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
  if (!ready_ || busy() || state_ != SecurityState::kProvisioned) return false;
  if (tx_reserved_bound_ > UINT64_MAX - kTxReservationBlockSize) {
    if (!exhausted_) ++diagnostics_.exhausted_events;
    exhausted_ = true;
    return false;  // FAIL CLOSED at exhaustion; no rollover invented here.
  }
  if (pages_[active_page_].tx_reserve_used + 1 >= kTxReserveSlotsPerPage) {
    // Bounded headroom: compact onto a fresh page (carrying the current
    // credential and bound forward) before the active page is ever
    // discovered completely full at the moment a reservation is needed.
    reserve_after_new_page_ = true;
    return startNewPage(/*critical=*/false, /*seed_reserve=*/true, credential_);
  }
  pending_tx_bound_ = tx_reserved_bound_ + kTxReservationBlockSize;
  TxReserve reserve{};
  memcpy(reserve.credential_id, credential_.credential_id, kCredentialIdSize);
  reserve.key_epoch = credential_.key_epoch;
  reserve.tx_reserved_bound = pending_tx_bound_;
  uint8_t bytes[kTxReserveRecordSize];
  encodeTxReserve(reserve, bytes);
  active_port_ = &critical_;
  target_page_ = static_cast<uint32_t>(active_page_);
  target_slot_ = pages_[active_page_].tx_reserve_used;
  job_ = Job::kReserve;
  phase_ = Phase::kWriteReserve;
  startBlob(reserveOffset(target_page_, target_slot_), bytes, sizeof(bytes));
  return true;
}

void SecurityStore::startBlob(uint32_t offset, const uint8_t* bytes, uint32_t size) {
  memcpy(blob_, bytes, size);
  blob_offset_ = offset;
  blob_size_ = size;
  blob_step_ = BlobStep::kBody;
  flash_op_awaiting_completion_ = false;
}

FlashOpResult SecurityStore::writeBlob() {
  FlashBackend& port = *active_port_;
  if (blob_step_ == BlobStep::kBody) {
    const FlashOpResult result = flash_op_awaiting_completion_
        ? port.pollPending()
        : port.program(blob_offset_, blob_, blob_size_ - 4);
    if (result == FlashOpResult::kPending) { flash_op_awaiting_completion_ = true; return FlashOpResult::kPending; }
    flash_op_awaiting_completion_ = false;
    if (result == FlashOpResult::kFailed) { fail(); return FlashOpResult::kFailed; }
    blob_step_ = BlobStep::kCommit;
  }
  if (blob_step_ == BlobStep::kCommit) {
    const FlashOpResult result = flash_op_awaiting_completion_
        ? port.pollPending()
        : port.program(blob_offset_ + blob_size_ - 4, blob_ + blob_size_ - 4, 4);
    if (result == FlashOpResult::kPending) { flash_op_awaiting_completion_ = true; return FlashOpResult::kPending; }
    flash_op_awaiting_completion_ = false;
    if (result == FlashOpResult::kFailed) { fail(); return FlashOpResult::kFailed; }
    blob_step_ = BlobStep::kVerify;
  }
  uint8_t verify[kCredentialRecordSize];
  if (!port.read(blob_offset_, verify, blob_size_) || memcmp(verify, blob_, blob_size_) != 0) {
    blob_step_ = BlobStep::kBody;
    fail();
    return FlashOpResult::kFailed;
  }
  blob_step_ = BlobStep::kBody;
  return FlashOpResult::kDone;
}

void SecurityStore::fail() {
  const Job failing_job = job_;
  const bool was_seed_reserve = seed_reserve_;
  job_ = Job::kNone;
  phase_ = Phase::kErasePage;
  blob_step_ = BlobStep::kBody;
  flash_op_awaiting_completion_ = false;
  reserve_after_new_page_ = false;
  // Deliberately does NOT touch credential_/tx_reserved_bound_/tx_next_/
  // active_page_/state_: every failure path here targeted the currently
  // INACTIVE page or an append slot beyond the already-committed state, so
  // the previously committed page/counters remain untouched and
  // authoritative, exactly like ConfigStore::fail().
  if (failing_job == Job::kReserve) {
    ++diagnostics_.reservation_failures;
  } else if (failing_job == Job::kNewPage) {
    if (!was_seed_reserve) {
      commit_result_ready_ = true;
      commit_success_ = false;
      ++diagnostics_.commit_failures;
    } else {
      ++diagnostics_.reservation_failures;
    }
  }
}

void SecurityStore::completeNewPage() {
  pages_[target_page_].generation = target_generation_;
  pages_[target_page_].tx_reserve_used = seed_reserve_ ? 1 : 0;
  active_page_ = static_cast<int>(target_page_);
  newest_generation_ = target_generation_;
  credential_ = pending_credential_;
  state_ = SecurityState::kProvisioned;
  job_ = Job::kNone;
  if (seed_reserve_) {
    ++diagnostics_.compactions;
  } else {
    tx_reserved_bound_ = 0;
    tx_next_ = 0;
    exhausted_ = false;
    ++diagnostics_.commits;
    commit_result_ready_ = true;
    commit_success_ = true;
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
  maybeAutoReserve();
}

void SecurityStore::completeReserve() {
  pages_[target_page_].tx_reserve_used = target_slot_ + 1;
  tx_reserved_bound_ = pending_tx_bound_;
  job_ = Job::kNone;
  ++diagnostics_.reservations;
}

void SecurityStore::maybeAutoReserve() {
  if (job_ == Job::kNone && state_ == SecurityState::kProvisioned && !exhausted_ &&
      tx_next_ == tx_reserved_bound_) {
    startReservation();
  }
}

void SecurityStore::poll() {
  if (!ready_) return;
  if (job_ == Job::kNone) {
    maybeAutoReserve();
    return;
  }

  if (phase_ == Phase::kErasePage) {
    const FlashOpResult result = flash_op_awaiting_completion_
        ? active_port_->pollPending()
        : active_port_->erasePage(target_page_);
    if (result == FlashOpResult::kPending) { flash_op_awaiting_completion_ = true; return; }
    flash_op_awaiting_completion_ = false;
    if (result == FlashOpResult::kFailed) { fail(); return; }
    phase_ = Phase::kWriteHeader;
    PageHeader header{target_generation_, device_identity_.legacyUint64()};
    uint8_t bytes[kPageHeaderSize];
    encodePageHeader(header, bytes);
    startBlob(headerOffset(target_page_), bytes, sizeof(bytes));
    return;
  }

  if (phase_ == Phase::kEraseOldPage) {
    const FlashOpResult result = flash_op_awaiting_completion_
        ? active_port_->pollPending()
        : active_port_->erasePage(target_page_);
    if (result == FlashOpResult::kPending) { flash_op_awaiting_completion_ = true; return; }
    flash_op_awaiting_completion_ = false;
    completeEraseOld(result == FlashOpResult::kDone);
    return;
  }

  if (writeBlob() != FlashOpResult::kDone) return;

  if (phase_ == Phase::kWriteHeader) {
    if (seed_reserve_) {
      // Compaction snapshot ordering is security-critical: the new page's
      // credential is its effective activation record for recovery. Carry
      // the already-durable TX high-water mark first, then commit the
      // credential last. A reset at any earlier point therefore leaves the
      // new page incomplete and the old page authoritative; recovery can
      // never select a higher-generation credential page that lost its
      // counter bound.
      phase_ = Phase::kWriteReserve;
      TxReserve reserve{};
      memcpy(reserve.credential_id, pending_credential_.credential_id, kCredentialIdSize);
      reserve.key_epoch = pending_credential_.key_epoch;
      reserve.tx_reserved_bound = pending_tx_bound_;
      uint8_t bytes[kTxReserveRecordSize];
      encodeTxReserve(reserve, bytes);
      startBlob(reserveOffset(target_page_, 0), bytes, sizeof(bytes));
      return;
    }
    phase_ = Phase::kWriteCredential;
    uint8_t bytes[kCredentialRecordSize];
    encodeCredential(pending_credential_, bytes);
    startBlob(credentialOffset(target_page_), bytes, sizeof(bytes));
    return;
  }
  if (phase_ == Phase::kWriteCredential) {
    completeNewPage();
    return;
  }
  if (phase_ == Phase::kWriteReserve) {
    if (job_ == Job::kNewPage) {
      phase_ = Phase::kWriteCredential;
      uint8_t bytes[kCredentialRecordSize];
      encodeCredential(pending_credential_, bytes);
      startBlob(credentialOffset(target_page_), bytes, sizeof(bytes));
      return;
    }
    completeReserve();
    return;
  }
}

}  // namespace orun_tlp
