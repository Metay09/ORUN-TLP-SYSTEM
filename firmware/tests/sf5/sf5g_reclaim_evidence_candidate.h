#pragma once

// SF5G HOST-ONLY advisory record coverage check. NEVER an erase/ACK API.
// This module cannot authenticate any receipt; it cannot prove custody
// release or authorize reuse of physical NOR. No production code links it.
#include <stddef.h>
#include <stdint.h>

#include "sf5g_nor_page_writer_candidate.h"

namespace orun_tlp {
namespace sf5g_reclaim_evidence_candidate {

namespace nor = sf5g_nor_writer_candidate;
namespace record = sf5g_custody_candidate;
namespace format = sf5g_page_candidate;

// The caller's receipt source is NOT intrinsically trusted here. A future
// protocol verifier must implement actual authorization, replay protection,
// cryptographic receipt binding and durable Edge acceptance. Tests provide
// a mock source only. This interface produces NO erase authorization.
enum class Claim : uint8_t {
  kAbsent,
  kClaimsDurableExactObject,
  kInvalid,
};

// Exact on-flash sealed-object bytes and physical identity are supplied
// in each query. Neither CRC nor the shape check proves authenticity.
class ReceiptSource {
 public:
  virtual ~ReceiptSource() {}
  virtual Claim lookup(uint16_t page_index, uint64_t generation,
                       uint16_t slot_index, const uint8_t* exact_object,
                       size_t exact_object_size) const = 0;
};

enum class ReviewState : uint8_t {
  kCandidateOnly,        // mock claims match ALL 32 handed-off entries;
                         // explicitly NOT permission to erase
  kNotFull,              // valid page but unfilled, do not reclaim
  kHeld,                 // at least one still HELD or uncertain marker
  kMissingReceipt,       // missing one or more exact/identity-bound claims
  kRefused,              // foreign/torn/invalid/ambiguous on-flash evidence
  kReadFailure,
};

struct Review {
  ReviewState state = ReviewState::kRefused;
  uint64_t generation = 0U;
  uint16_t checked = 0U;
  uint16_t first_blocked_slot = UINT16_MAX;
};

// Inspects exact 4 KiB page snapshot, validates OCP3 header and *all*
// record slots, requires a full page and canonical handoff marker state,
// then consults ReceiptSource for each physical slot. This is a strict
// host-only diagnostic, and the result must NOT be used as erase authority.
// Source index/count are structural checks; product_region ownership,
// generation allocation policy and authenticated Edge receipts remain open.
Review reviewPage(const nor::NorPage& physical, uint16_t source_page_index,
                  uint16_t actual_page_count, const ReceiptSource& claims);

}  // namespace sf5g_reclaim_evidence_candidate
}  // namespace orun_tlp
