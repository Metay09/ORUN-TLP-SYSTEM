#include "kq05_gateway_receipt_journal.h"

#include <string.h>

namespace orun_tlp {
namespace {
namespace fmt = kq05_receipt_journal;

bool allErased(const uint8_t* p, size_t length) {
  for (size_t n = 0; n < length; ++n)
    if (p[n] != 0xFFU) return false;
  return true;
}

}  // namespace

bool Kq05GatewayReceiptJournal::inSafeBounds(
    uint32_t offset, size_t length) const {
  const uint32_t limit = fmt::kReceiptPageCount * fmt::kPageSize;
  return offset <= limit && length <= limit - offset;
}

uint32_t Kq05GatewayReceiptJournal::slotOffset(
    uint32_t page, uint32_t slot) const {
  return page * fmt::kPageSize + fmt::kPageHeaderSize +
         slot * fmt::kRecordSize;
}

bool Kq05GatewayReceiptJournal::readExact(
    uint32_t offset, const uint8_t* expected, size_t length) const {
  if (expected == nullptr || length > fmt::kRecordSize ||
      !inSafeBounds(offset, length)) return false;
  uint8_t scratch[fmt::kRecordSize]{};
  return flash_.read(offset, scratch, length) &&
         memcmp(expected, scratch, length) == 0;
}

bool Kq05GatewayReceiptJournal::commitBodyAndMarker(
    uint32_t offset, const uint8_t* body, size_t body_length,
    const uint8_t (&marker)[4], size_t full_length) {
  if (body == nullptr || full_length != body_length + 4U ||
      full_length > fmt::kRecordSize ||
      !inSafeBounds(offset, full_length) ||
      flash_.hasUnreconciledMutation()) return false;
  // Never resubmit after kPending: the bench-only owner is not an async
  // FlashMutationGate client and must NOT be installed on live BLE firmware.
  if (flash_.program(offset, body, body_length) != FlashOpResult::kDone ||
      !readExact(offset, body, body_length))
    return false;
  if (flash_.program(offset + static_cast<uint32_t>(body_length),
                     marker, 4U) != FlashOpResult::kDone ||
      !readExact(offset + static_cast<uint32_t>(body_length), marker, 4U))
    return false;
  return true;
}

bool Kq05GatewayReceiptJournal::scan() {
  count_ = 0U;
  occupied_ = 0U;
  memset(page_committed_, 0, sizeof(page_committed_));
  memset(slots_used_, 0, sizeof(slots_used_));
  state_ = Mount::kFault;

  bool saw_uninitialized = false;
  for (uint32_t page = 0; page < fmt::kReceiptPageCount; ++page) {
    uint8_t header[fmt::kPageHeaderSize]{};
    const uint32_t base = page * fmt::kPageSize;
    fmt::HeaderView head{};
    if (!flash_.read(base, header, sizeof(header))) return false;
    const auto evidence = fmt::inspectHeader(header, sizeof(header), head);
    if (evidence == fmt::Evidence::kErased) {
      saw_uninitialized = true;
      // A page whose header is erased must contain no leftover old data,
      // stale records, partial erase or different writer's payload.
      uint8_t cell[fmt::kRecordSize]{};
      for (uint32_t slot = 0U; slot < fmt::kRecordsPerPage; ++slot) {
        const uint32_t offset = slotOffset(page, slot);
        if (!flash_.read(offset, cell, sizeof(cell)) ||
            !allErased(cell, sizeof(cell)))
          return false;
      }
      continue;
    }
    if (evidence != fmt::Evidence::kCommitted) return false;
    if (head.gateway_id != gateway_id_) {
      state_ = Mount::kForeign;
      return false;
    }
    // Without reclaim, each physical page is prepared at most once.
    if (saw_uninitialized || head.generation != page + 1U) return false;
    page_committed_[page] = true;
    bool seen_empty = false;
    for (uint32_t slot = 0U; slot < fmt::kRecordsPerPage; ++slot) {
      uint8_t cell[fmt::kRecordSize]{};
      uint8_t offer[fmt::kOfferMax]{};
      size_t offer_size = 0U;
      if (!flash_.read(slotOffset(page, slot), cell, sizeof(cell)))
        return false;
      const auto record = fmt::inspectRecord(
          cell, sizeof(cell), offer, offer_size);
      if (record == fmt::Evidence::kErased) {
        seen_empty = true;
        continue;
      }
      if (seen_empty || record == fmt::Evidence::kCorrupt)
        return false;
      // Torn slot consumes physical capacity; it is never a receipt.
      ++slots_used_[page];
      ++occupied_;
      if (record == fmt::Evidence::kCommitted) ++count_;
    }
  }
  state_ = page_committed_[0] ? Mount::kReady : Mount::kEmpty;
  return true;
}

Kq05GatewayReceiptJournal::Mount
Kq05GatewayReceiptJournal::mountReadOnly() {
  state_ = Mount::kFault;
  if (gateway_id_ == 0U || !flash_.begin() ||
      flash_.hasUnreconciledMutation() || !scan())
    return state_;
  return state_;
}

bool Kq05GatewayReceiptJournal::preparePage(uint32_t page) {
  if (state_ != Mount::kReady && state_ != Mount::kEmpty)
    return false;
  if (page >= fmt::kReceiptPageCount || page_committed_[page] ||
      (page > 0U && (!page_committed_[page - 1U] ||
                     slots_used_[page - 1U] != fmt::kRecordsPerPage)))
    return false;
  uint8_t header[fmt::kPageHeaderSize]{};
  if (!fmt::encodeHeader(gateway_id_, page + 1U, header))
    return false;
  uint8_t marker[4]{};
  fmt::headerCommitMarker(marker);
  const uint32_t base = page * fmt::kPageSize;
  if (!commitBodyAndMarker(base, header, 60U, marker,
                           sizeof(header)))
    return false;
  uint8_t verified[fmt::kPageHeaderSize]{};
  fmt::HeaderView view{};
  if (!flash_.read(base, verified, sizeof(verified)) ||
      fmt::inspectHeader(verified, sizeof(verified), view) !=
          fmt::Evidence::kCommitted ||
      view.gateway_id != gateway_id_ ||
      view.generation != page + 1U)
    return false;
  page_committed_[page] = true;
  state_ = Mount::kReady;
  return true;
}

bool Kq05GatewayReceiptJournal::initializeErased() {
  if (state_ != Mount::kEmpty || page_committed_[0] ||
      occupied_ != 0U ||
      !preparePage(0U)) {
    state_ = Mount::kFault;
    return false;
  }
  return true;
}

bool Kq05GatewayReceiptJournal::findSourceRow(
    const uint8_t* logical_offer, size_t length,
    bool& duplicate, bool& conflict) const {
  duplicate = false;
  conflict = false;
  for (uint32_t page = 0; page < fmt::kReceiptPageCount; ++page) {
    for (uint32_t slot = 0; slot < slots_used_[page]; ++slot) {
      uint8_t cell[fmt::kRecordSize]{};
      uint8_t stored[fmt::kOfferMax]{};
      size_t stored_len = 0U;
      if (!flash_.read(slotOffset(page, slot), cell, sizeof(cell)))
        return false;
      const auto evidence = fmt::inspectRecord(
          cell, sizeof(cell), stored, stored_len);
      if (evidence == fmt::Evidence::kIncomplete)
        continue;
      if (evidence != fmt::Evidence::kCommitted)
        return false;
      if (memcmp(stored + 11U, logical_offer + 11U, 8U) == 0) {
        if (stored_len == length &&
            memcmp(stored, logical_offer, length) == 0)
          duplicate = true;
        else
          conflict = true;
      }
    }
  }
  return true;
}

Kq05GatewayReceiptJournal::Put
Kq05GatewayReceiptJournal::putVerified(
    const uint8_t* logical_offer, size_t length,
    bool authenticated_admission) {
  if (!authenticated_admission) return Put::kRejected;
  if (state_ != Mount::kReady || flash_.hasUnreconciledMutation())
    return Put::kFault;
  uint8_t encoded[fmt::kRecordSize]{};
  if (!fmt::encodeRecord(logical_offer, length, encoded))
    return Put::kRejected;

  bool duplicate = false;
  bool conflict = false;
  if (!findSourceRow(logical_offer, length, duplicate, conflict)) {
    state_ = Mount::kFault;
    return Put::kFault;
  }
  if (conflict) return Put::kConflict;
  if (duplicate) return Put::kDuplicate;

  uint32_t page = 0U;
  while (page < fmt::kReceiptPageCount &&
         slots_used_[page] == fmt::kRecordsPerPage) ++page;
  if (page == fmt::kReceiptPageCount) return Put::kFull;
  if (!page_committed_[page] && !preparePage(page)) {
    state_ = Mount::kFault;
    return Put::kFault;
  }
  const uint32_t offset = slotOffset(page, slots_used_[page]);
  uint8_t marker[4]{};
  fmt::recordCommitMarker(marker);
  if (!commitBodyAndMarker(offset, encoded, 140U, marker,
                           sizeof(encoded))) {
    state_ = Mount::kFault;
    return Put::kFault;
  }
  uint8_t readback[fmt::kRecordSize]{};
  uint8_t recovered[fmt::kOfferMax]{};
  size_t recovered_size = 0U;
  if (!flash_.read(offset, readback, sizeof(readback)) ||
      fmt::inspectRecord(readback, sizeof(readback),
                         recovered, recovered_size) !=
          fmt::Evidence::kCommitted ||
      recovered_size != length ||
      memcmp(recovered, logical_offer, length) != 0) {
    state_ = Mount::kFault;
    return Put::kFault;
  }
  ++slots_used_[page];
  ++count_;
  ++occupied_;
  return Put::kStored;
}

bool Kq05GatewayReceiptJournal::read(
    size_t index, uint8_t (&offer)[fmt::kOfferMax],
    size_t& length) const {
  if (state_ != Mount::kReady || index >= count_)
    return false;
  size_t seen = 0U;
  for (uint32_t page = 0; page < fmt::kReceiptPageCount; ++page) {
    for (uint32_t slot = 0; slot < slots_used_[page]; ++slot) {
      uint8_t cell[fmt::kRecordSize]{};
      uint8_t result[fmt::kOfferMax]{};
      size_t n = 0U;
      if (!flash_.read(slotOffset(page, slot), cell, sizeof(cell)))
        return false;
      const auto e = fmt::inspectRecord(cell, sizeof(cell), result, n);
      if (e == fmt::Evidence::kIncomplete) continue;
      if (e != fmt::Evidence::kCommitted) return false;
      if (seen++ == index) {
        memcpy(offer, result, n);
        length = n;
        return true;
      }
    }
  }
  return false;
}

}  // namespace orun_tlp
