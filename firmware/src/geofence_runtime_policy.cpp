#include "geofence_runtime_policy.h"

#include <limits.h>

namespace orun_tlp {
namespace geofence_runtime_policy {

uint32_t effectiveTrackingIntervalMs(uint32_t base_interval_seconds,
                                     GeofenceCadenceMode mode) {
  if (base_interval_seconds == 0) return 0;

  uint32_t seconds = base_interval_seconds;
  if (mode == GeofenceCadenceMode::kBaseDividedBy3) {
    seconds = base_interval_seconds / 3U;
    if ((base_interval_seconds % 3U) != 0U) ++seconds;
    if (seconds == 0) seconds = 1;
  }

  if (seconds > UINT32_MAX / 1000U) return 0;
  return seconds * 1000U;
}

bool selectRepresentative(const RepresentativeCandidate* candidates,
                          uint8_t count,
                          PermittedAreaRelation winning_relation,
                          uint8_t* observation_index) {
  if (candidates == nullptr || observation_index == nullptr || count == 0 ||
      (winning_relation != PermittedAreaRelation::kInside &&
       winning_relation != PermittedAreaRelation::kOutside)) {
    return false;
  }

  bool found = false;
  RepresentativeCandidate best;
  for (uint8_t i = 0; i < count; ++i) {
    const RepresentativeCandidate& candidate = candidates[i];
    if (candidate.relation != winning_relation) continue;

    if (!found || candidate.hdop_x100 < best.hdop_x100 ||
        (candidate.hdop_x100 == best.hdop_x100 &&
         candidate.satellites > best.satellites) ||
        (candidate.hdop_x100 == best.hdop_x100 &&
         candidate.satellites == best.satellites &&
         candidate.observation_index > best.observation_index)) {
      best = candidate;
      found = true;
    }
  }

  if (!found) return false;
  *observation_index = best.observation_index;
  return true;
}

}  // namespace geofence_runtime_policy
}  // namespace orun_tlp
