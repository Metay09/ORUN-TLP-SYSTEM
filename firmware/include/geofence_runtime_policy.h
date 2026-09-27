#pragma once

#include <stdint.h>

#include "geofence_area_set.h"
#include "geofence_operational_state.h"

namespace orun_tlp::geofence_runtime_policy {

uint32_t effectiveTrackingIntervalMs(uint32_t base_interval_seconds,
                                     GeofenceCadenceMode mode);

struct RepresentativeCandidate {
  constexpr RepresentativeCandidate(
      PermittedAreaRelation relation_value = PermittedAreaRelation::kInvalidPoint,
      uint16_t hdop_x100_value = UINT16_MAX,
      uint8_t satellites_value = 0,
      uint8_t observation_index_value = 0)
      : relation(relation_value),
        hdop_x100(hdop_x100_value),
        satellites(satellites_value),
        observation_index(observation_index_value) {}

  PermittedAreaRelation relation;
  uint16_t hdop_x100;
  uint8_t satellites;
  uint8_t observation_index;
};

bool selectRepresentative(const RepresentativeCandidate* candidates,
                          uint8_t count,
                          PermittedAreaRelation winning_relation,
                          uint8_t* observation_index);

}  // namespace orun_tlp::geofence_runtime_policy
