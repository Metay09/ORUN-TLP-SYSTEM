#pragma once
#include <stdint.h>
#include "flash_backend.h"

// SF5K3G: real FlashBackend read-only A/B boot-selection preflight.
// Used by SF5K3I SecurityStore for v3 boot selection, but exposes NO
// K_root/Gateway identities, and calls no
// program/erase calls. The active SecurityStore keeps v1/v2 authority.
namespace orun_tlp {
namespace security_v3_ab_preflight {

enum class Result : uint8_t {
  kUnprovisioned, kV1, kV2, kV3, kUnsupported, kForeign, kFault, kReadFault
};

struct Summary {
  uint8_t selected_page = 0xFFU;
  uint8_t selected_version = 0U;
  uint64_t generation = 0U;
  uint64_t tx_reserved_bound = 0U;
  uint64_t a2d_replay_bound = 0U;
  uint32_t policy_revision = 0U;
  uint32_t custody_floor = 0U;
  uint32_t max_grant_generation = 0U;
  uint8_t active_gateway_count = 0U;
};

// Requires an already initialized and QUIESCENT FlashBackend: no in-flight
// program/erase, no concurrent writer or pending SoftDevice flash completion.
// Scans the exact two existing 4096-byte SecurityStore pages using bounded
// buffers and independently validates legacy v1/v2 or v3.
// Only a fully selected version publishes Summary. No operation mutates flash.
// Result kV3 is NOT a Gateway ACK authorization decision.
Result inspect(const FlashBackend& flash, uint64_t tracker_id, Summary& out);

}  // namespace security_v3_ab_preflight
}  // namespace orun_tlp
