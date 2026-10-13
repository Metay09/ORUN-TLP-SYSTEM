#include "sf5g_intent_census_candidate.h"

#include <array>

namespace orun_tlp {
namespace sf5g_intent_census_candidate {
namespace {
constexpr size_t kIntentAreaOffset =
    format::kPageSize - format::kIntentSize * format::kIntentsPerPage;
static_assert(kIntentAreaOffset == 4024U, "unexpected intent tail offset");
}

JournalReport inspectJournal(const nor::NorPage& source,
                             uint16_t source_page_index,
                             uint16_t actual_page_count) {
  JournalReport report;
  if (actual_page_count == 0U ||
      actual_page_count > format::kMaxRegionPages ||
      source_page_index >= actual_page_count) return report;

  std::array<uint8_t, format::kIntentSize * format::kIntentsPerPage> bytes{};
  if (!source.read(kIntentAreaOffset, bytes.data(), bytes.size())) {
    report.state = JournalState::kReadFailure;
    return report;
  }

  // Exactly: [COMPLETED]*, [COMMITTED]?, [ERASED]*.
  // Interrupted staging, partially written commit/done, untrusted CRC or
  // noncontiguous slots fail closed; a pending intent is NOT erase proof.
  bool saw_free = false;
  bool saw_pending = false;
  for (size_t i = 0U; i < format::kIntentsPerPage; ++i) {
    format::IntentInspection item;
    if (!format::inspectIntent(bytes.data() + i * format::kIntentSize,
                               format::kIntentSize, item))
      return JournalReport();

    if (item.evidence == format::IntentEvidence::kErased) {
      if (!saw_free) report.next_free_slot = static_cast<uint8_t>(i);
      saw_free = true;
      ++report.erased;
      continue;
    }

    if (saw_free || saw_pending) return JournalReport();
    if (item.evidence != format::IntentEvidence::kCommitted &&
        item.evidence != format::IntentEvidence::kCompleted)
      return JournalReport();

    // Independent physical region and self-target guards, not an erase
    // permission or identity proof. Target generation only has valid shape.
    if (item.target_page >= actual_page_count ||
        item.target_page == source_page_index)
      return JournalReport();

    if (item.evidence == format::IntentEvidence::kCompleted) {
      ++report.completed;
      continue;
    }
    saw_pending = true;
    report.pending_target_page = item.target_page;
    report.pending_target_generation = item.target_generation;
  }

  if (saw_pending) {
    report.state = JournalState::kPending;
  } else if (report.erased == 0U) {
    report.state = JournalState::kExhausted;
  } else {
    report.state = JournalState::kAvailable;
  }
  return report;
}

}  // namespace sf5g_intent_census_candidate
}  // namespace orun_tlp
