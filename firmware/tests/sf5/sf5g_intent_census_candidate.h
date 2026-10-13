#pragma once

// Host-only SF5G reclaim intent AREA CENSUS.
// Read-only diagnostic classifier, NEVER authorization to erase, reclaim,
// complete an intent, release a packet, or issue custody ACK.
#include <stddef.h>
#include <stdint.h>

#include "sf5g_nor_page_writer_candidate.h"

namespace orun_tlp {
namespace sf5g_intent_census_candidate {

namespace nor = sf5g_nor_writer_candidate;
namespace format = sf5g_page_candidate;

// A completed intent is only structurally completed. It does not prove
// that its targeted page was erased or had authenticated durable handoff.
enum class JournalState : uint8_t {
  kAvailable,   // Completed prefix + erased suffix, at least one free slot
  kPending,     // One valid committed (NOT completed) intent
  kExhausted,   // All three intents structurally completed, zero free slots
  kRefused,     // Torn/corrupt/out-of-order/foreign/out-of-region/self-target
  kReadFailure,
};

struct JournalReport {
  JournalState state = JournalState::kRefused;
  uint8_t completed = 0U;
  uint8_t erased = 0U;
  uint8_t next_free_slot = UINT8_MAX;
  uint16_t pending_target_page = UINT16_MAX;
  uint64_t pending_target_generation = 0U;
};

// Reads ONLY the final 72 bytes of a candidate OCP3 page. This is NOT
// source-header/owner verification, nor target-page recovery validation.
// A caller CANNOT turn these diagnostics into flash erase authorization.
// Requires externally provided physical region size and source page index.
JournalReport inspectJournal(const nor::NorPage& source,
                             uint16_t source_page_index,
                             uint16_t actual_page_count);

}  // namespace sf5g_intent_census_candidate
}  // namespace orun_tlp
