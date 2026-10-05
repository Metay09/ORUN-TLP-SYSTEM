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
    const tlp::BackendDurableReceiptPlaintext& receipt) {
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

  // Work on a scratch image first. An invalid/unknown/over-capacity receipt
  // must not partially change the coordinator or HistoryStore RAM watermark.
  uint64_t scratch[kMaxSelectiveAcknowledgements]{};
  size_t scratch_count = selective_count_;
  memcpy(scratch, selective_ids_, sizeof(scratch));

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

  bool new_fact = false;

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

    if (!insertIdentitySorted(
            scratch, scratch_count,
            kMaxSelectiveAcknowledgements, id)) {
      ++diagnostics_.capacity_rejections;
      return ApplyResult::kSelectiveSetFull;
    }
    new_fact = true;

    // Advance only through actual retained records whose exact identities have
    // already been authenticated and admitted into the selective RAM set.
    while (true) {
      HistoryStore::Record next{};
      if (!history_.readAfter(simulated_acknowledged, next)) break;

      size_t next_index = 0;
      if (!findIdentity(
              scratch, scratch_count, next.identity, &next_index)) {
        break;
      }

      simulated_acknowledged = next.identity;
      eraseIdentityAt(scratch, scratch_count, next_index);
    }
  }

  // Commit the preflighted contiguous prefix to HistoryStore RAM only.
  uint32_t advances = 0;
  while (history_.acknowledgedThrough() < simulated_acknowledged) {
    HistoryStore::Record next{};
    if (!history_.readAfter(history_.acknowledgedThrough(), next) ||
        next.identity > simulated_acknowledged ||
        !history_.acknowledgeDeliveredRecord(next.identity)) {
      ++diagnostics_.invariant_failures;
      return ApplyResult::kInvariantFailure;
    }
    ++advances;
  }

  memcpy(selective_ids_, scratch, sizeof(selective_ids_));
  selective_count_ = scratch_count;
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
