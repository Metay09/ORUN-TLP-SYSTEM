#include "geofence_area_set.h"

namespace orun_tlp {

PermittedAreaAssessment assessPermittedGeofenceAreas(
    const GeofenceAreaSetView& area_set, const GeoPointE7& point) {
  if (area_set.areas == nullptr || area_set.area_count == 0) {
    return PermittedAreaAssessment(PermittedAreaRelation::kInvalidAreaSet);
  }

  // Validate the entire configured set first. Returning INSIDE from an early
  // polygon while silently ignoring a malformed later polygon would turn a
  // configuration fault into order-dependent behavior.
  for (uint16_t i = 0; i < area_set.area_count; ++i) {
    if (validateGeofencePolygon(area_set.areas[i]) !=
        GeofencePolygonValidation::kOk) {
      return PermittedAreaAssessment(PermittedAreaRelation::kInvalidAreaSet, i);
    }
  }

  uint16_t boundary_index = kNoGeofenceAreaIndex;
  for (uint16_t i = 0; i < area_set.area_count; ++i) {
    const GeofencePointRelation relation =
        classifyPointInGeofencePolygon(area_set.areas[i], point);
    if (relation == GeofencePointRelation::kInvalidPoint) {
      return PermittedAreaAssessment(PermittedAreaRelation::kInvalidPoint);
    }
    // Should be unreachable after the full validation pass, but keep the
    // composition fail-closed if the geometry contract changes later.
    if (relation == GeofencePointRelation::kInvalidPolygon) {
      return PermittedAreaAssessment(PermittedAreaRelation::kInvalidAreaSet, i);
    }
    if (relation == GeofencePointRelation::kInside) {
      return PermittedAreaAssessment(PermittedAreaRelation::kInside, i);
    }
    if (relation == GeofencePointRelation::kBoundary &&
        boundary_index == kNoGeofenceAreaIndex) {
      boundary_index = i;
    }
  }

  if (boundary_index != kNoGeofenceAreaIndex) {
    return PermittedAreaAssessment(PermittedAreaRelation::kBoundary,
                                   boundary_index);
  }
  return PermittedAreaAssessment(PermittedAreaRelation::kOutside);
}

}  // namespace orun_tlp
