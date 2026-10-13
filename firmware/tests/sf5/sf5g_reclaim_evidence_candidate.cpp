#include "sf5g_reclaim_evidence_candidate.h"

#include <array>

namespace orun_tlp {
namespace sf5g_reclaim_evidence_candidate {
namespace {
constexpr size_t kRecordOffset = format::kHeaderSize;
constexpr size_t kEndOfRecords =
    kRecordOffset + format::kRecordsPerPage * format::kRecordSize;
static_assert(kEndOfRecords == 3904U, "unexpected record-area end");

bool allErased(const uint8_t* bytes, size_t size) {
  for (size_t i = 0U; i < size; ++i)
    if (bytes[i] != 0xFFU) return false;
  return true;
}
uint32_t getMarker(const uint8_t* bytes) {
  return (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) |
         (uint32_t(bytes[2]) << 8) | uint32_t(bytes[3]);
}

// Conservative current format: handoff slot A must be fully committed
// first, slot B may be fully erased or committed. Partial markers and
// B-only completion are not interpretable as release evidence.
bool canonicalHandoff(const uint8_t* bytes) {
  const uint32_t a =
      getMarker(bytes + record::kRecordHandoff0Offset);
  const uint32_t b =
      getMarker(bytes + record::kRecordHandoff1Offset);
  return a == record::kProgrammedMarker &&
         (b == record::kErasedMarker || b == record::kProgrammedMarker);
}
}  // namespace

Review reviewPage(const nor::NorPage& physical, uint16_t source_page_index,
                  uint16_t actual_page_count, const ReceiptSource& claims) {
  Review answer;
  if (actual_page_count == 0U ||
      actual_page_count > format::kMaxRegionPages ||
      source_page_index >= actual_page_count)
    return answer;

  std::array<uint8_t, format::kPageSize> snapshot{};
  if (!physical.read(0U, snapshot.data(), snapshot.size())) {
    answer.state = ReviewState::kReadFailure;
    return answer;
  }

  format::HeaderInspection header;
  if (!format::inspectHeader(snapshot.data(), format::kHeaderSize, header) ||
      header.evidence != format::HeaderEvidence::kActive ||
      !allErased(snapshot.data() + kEndOfRecords,
                 snapshot.size() - kEndOfRecords))
    return answer;
  answer.generation = header.generation;

  bool any_erased = false;
  bool any_held = false;
  for (size_t slot = 0U; slot < format::kRecordsPerPage; ++slot) {
    const uint8_t* raw =
        snapshot.data() + kRecordOffset + slot * format::kRecordSize;
    record::RecordInspection inspected;
    if (!record::inspectRecord(raw, format::kRecordSize, inspected))
      return Review();
    if (inspected.evidence == record::Evidence::kErased) {
      any_erased = true;
    } else if (inspected.evidence == record::Evidence::kHeld) {
      if (any_erased) return Review();  // a hole preceding real records
      any_held = true;
      if (answer.first_blocked_slot == UINT16_MAX)
        answer.first_blocked_slot = static_cast<uint16_t>(slot);
    } else if (inspected.evidence == record::Evidence::kHandedOff) {
      if (any_erased || !canonicalHandoff(raw)) return Review();
    } else {
      return Review();  // staged/torn/corrupt is never reclaimable
    }
  }
  // No queries to a mock oracle until ALL slots have been validated.
  if (any_erased) {
    answer.state = ReviewState::kNotFull;
    return answer;
  }
  if (any_held) {
    answer.state = ReviewState::kHeld;
    return answer;
  }

  for (size_t slot = 0U; slot < format::kRecordsPerPage; ++slot) {
    const uint8_t* raw =
        snapshot.data() + kRecordOffset + slot * format::kRecordSize;
    record::RecordInspection inspected;
    if (!record::inspectRecord(raw, format::kRecordSize, inspected) ||
        inspected.evidence != record::Evidence::kHandedOff ||
        !canonicalHandoff(raw))
      return Review();
    const Claim claim = claims.lookup(
        source_page_index, header.generation, static_cast<uint16_t>(slot),
        inspected.object, inspected.object_size);
    ++answer.checked;
    if (claim != Claim::kClaimsDurableExactObject) {
      answer.state = claim == Claim::kInvalid
                         ? ReviewState::kRefused
                         : ReviewState::kMissingReceipt;
      answer.first_blocked_slot = static_cast<uint16_t>(slot);
      return answer;
    }
  }

  // Deliberately does not return any erase permission, capability or token.
  // A caller-controlled mock claim is not authenticated acceptance.
  answer.state = ReviewState::kCandidateOnly;
  return answer;
}

}  // namespace sf5g_reclaim_evidence_candidate
}  // namespace orun_tlp
