#include "history_delivery_coordinator.h"

#include <string.h>

namespace orun_tlp {

bool HistoryDeliveryCoordinator::findIdentity(
    const uint64_t* ids, size_t count, uint64_t id, size_t* index) {
  for (size_t i = 0; i < count; ++i) {
    if (ids[i] != id) continue;
    if (index != nullptr) *index = i;
    return true;
  }
  return false;
}

void HistoryDeliveryCoordinator::eraseIdentityAt(
    uint64_t* ids, size_t& count, size_t index) {
  if (index >= count) return;
  for (size_t i = index + 1; i < count; ++i)
    ids[i - 1] = ids[i];
  --count;
  ids[count] = 0;
}

bool HistoryDeliveryCoordinator::insertIdentitySorted(
    uint64_t* ids, size_t& count, size_t capacity, uint64_t id) {
  if (findIdentity(ids, count, id)) return true;
  if (count >= capacity) return false;

  size_t insert_at = 0;
  while (insert_at < count && ids[insert_at] < id) ++insert_at;
  for (size_t i = count; i > insert_at; --i)
    ids[i] = ids[i - 1];
  ids[insert_at] = id;
  ++count;
  return true;
}

HistoryDeliveryCoordinator::ApplyResult
HistoryDeliveryCoordinator::applyReplayAcceptedBackendDurableReceipt(
    const tlp::BackendDurableReceiptPlaintext& receipt,
    uint64_t authenticated_history_incarnation) {
  if (!history_.ready() || history_.busy())
    return ApplyResult::kUnavailable;

  if (receipt.count == 0 ||
      receipt.count > tlp::kHistoryReceiptMaxIdentities)
    return ApplyResult::kInvalidReceipt;

  for (uint8_t i = 0; i < receipt.count; ++i) {
    if (receipt.history_record_identities[i] == 0 ||
        (i != 0 &&
         receipt.history_record_identities[i - 1] >=
             receipt.history_record_identities[i])) {
      return ApplyResult::kInvalidReceipt;
    }
  }

  const uint64_t current_incarnation = history_.incarnation();
  if (authenticated_history_incarnation == 0 ||
      current_incarnation == 0 ||
      authenticated_history_incarnation != current_incarnation) {
    return ApplyResult::kInvalidReceipt;
  }

  // Work on a scratch image first. A History re-baseline/incarnation change
  // invalidates every old RAM-only selective fact, but do not mutate the owned
  // set until this current-incarnation receipt itself successfully commits.
  uint64_t scratch[kMaxSelectiveAcknowledgements]{};
  size_t scratch_count = 0;
  if (bound_history_incarnation_ == current_incarnation) {
    scratch_count = selective_count_;
    memcpy(scratch, selective_ids_, sizeof(scratch));
  }

  const uint64_t initial_acknowledged = history_.acknowledgedThrough();
  uint64_t simulated_acknowledged = initial_acknowledged;
  uint32_t stale_prunes = 0;

  // Capacity overwrite can make an older selective fact irrelevant before its
  // gap closes. Dropping that RAM-only fact cannot delete data: the record is
  // already absent, and HistoryStore reports capacity loss separately.
  for (size_t i = 0; i < scratch_count;) {
    HistoryStore::Record retained{};
    if (scratch[i] <= simulated_acknowledged ||
        !history_.lookup(scratch[i], retained)) {
      eraseIdentityAt(scratch, scratch_count, i);
      ++stale_prunes;
      continue;
    }
    ++i;
  }

  // Drain any already-authenticated selective prefix before processing the new
  // receipt. This is required after capacity overwrite: the former missing
  // oldest record may no longer exist, making an already-selective record the
  // new oldest surviving actual record. A full selective set must not deadlock
  // merely because the receipt is a duplicate of that now-oldest record.
  auto drainSelectivePrefix = [&]() -> bool {
    while (scratch_count != 0) {
      HistoryStore::Record next{};
      if (!history_.readNextRetained(simulated_acknowledged, next))
        return false;

      size_t next_index = 0;
      if (!findIdentity(
              scratch, scratch_count, next.identity, &next_index)) {
        return true;
      }

      simulated_acknowledged = next.identity;
      eraseIdentityAt(scratch, scratch_count, next_index);
    }
    return true;
  };

  if (!drainSelectivePrefix()) {
    ++diagnostics_.invariant_failures;
    return ApplyResult::kInvariantFailure;
  }

  bool new_fact = simulated_acknowledged > initial_acknowledged;

  for (uint8_t i = 0; i < receipt.count; ++i) {
    const uint64_t id = receipt.history_record_identities[i];

    if (id <= simulated_acknowledged ||
        findIdentity(scratch, scratch_count, id)) {
      continue;
    }

    HistoryStore::Record retained{};
    if (!history_.lookup(id, retained)) {
      ++diagnostics_.unknown_identity_rejections;
      return ApplyResult::kUnknownIdentity;
    }

    // Strict traversal must agree with the looser lookup before any History RAM
    // mutation. Persistent slot corruption therefore fails in preflight rather
    // than being skipped by readAfter()/lookup().
    HistoryStore::Record next_actual{};
    if (!history_.readNextRetained(simulated_acknowledged, next_actual)) {
      ++diagnostics_.invariant_failures;
      return ApplyResult::kInvariantFailure;
    }

    // The exact next actual record does not need a selective-set slot. This is
    // important when the bounded set is full: an authenticated receipt for the
    // missing oldest record must be able to unlock and drain the waiting prefix
    // instead of being rejected merely because all selective slots are occupied.
    if (next_actual.identity == id) {
      simulated_acknowledged = id;
      new_fact = true;
    } else {
      if (!insertIdentitySorted(
              scratch, scratch_count,
              kMaxSelectiveAcknowledgements, id)) {
        ++diagnostics_.capacity_rejections;
        return ApplyResult::kSelectiveSetFull;
      }
      new_fact = true;
    }

    if (!drainSelectivePrefix()) {
      ++diagnostics_.invariant_failures;
      return ApplyResult::kInvariantFailure;
    }
  }

  // Commit the preflighted contiguous prefix to HistoryStore RAM only.
  //
  // A transient read fault can still appear after preflight. In that case
  // acknowledgeDeliveredRecord() may already have committed a shorter,
  // individually strict-validated safe prefix. Never roll that watermark back;
  // report kInvariantFailure and require the caller to re-read it.
  uint32_t advances = 0;
  while (history_.acknowledgedThrough() < simulated_acknowledged) {
    HistoryStore::Record next{};
    if (!history_.readNextRetained(history_.acknowledgedThrough(), next) ||
        next.identity > simulated_acknowledged ||
        !history_.acknowledgeDeliveredRecord(next.identity)) {
      ++diagnostics_.invariant_failures;
      return ApplyResult::kInvariantFailure;
    }
    ++advances;
  }

  memcpy(selective_ids_, scratch, sizeof(selective_ids_));
  selective_count_ = scratch_count;
  bound_history_incarnation_ = current_incarnation;
  diagnostics_.stale_selective_prunes += stale_prunes;
  diagnostics_.contiguous_advances += advances;

  if (!new_fact && advances == 0) {
    ++diagnostics_.duplicate_receipts;
    return ApplyResult::kDuplicateOnly;
  }

  ++diagnostics_.applied_receipts;
  return ApplyResult::kApplied;
}

}  // namespace orun_tlp
