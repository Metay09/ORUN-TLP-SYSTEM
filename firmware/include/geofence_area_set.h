#pragma once

#include <stdint.h>

#include "geofence_geometry.h"

namespace orun_tlp {

constexpr uint16_t kNoGeofenceAreaIndex = 0xFFFFU;

struct GeofenceAreaSetView {
  constexpr GeofenceAreaSetView(const GeofencePolygonView* areas_value = nullptr,
                                uint16_t area_count_value = 0)
      : areas(areas_value), area_count(area_count_value) {}

  const GeofencePolygonView* areas;
  uint16_t area_count;
};

enum class PermittedAreaRelation : uint8_t {
  kInside,
  kBoundary,
  kOutside,
  kInvalidAreaSet,
  kInvalidPoint,
};

struct PermittedAreaAssessment {
  constexpr PermittedAreaAssessment(
      PermittedAreaRelation relation_value = PermittedAreaRelation::kInvalidAreaSet,
      uint16_t area_index_value = kNoGeofenceAreaIndex)
      : relation(relation_value), area_index(area_index_value) {}

  PermittedAreaRelation relation;
  // For kInside/kBoundary this is one matching permitted area. For
  // kInvalidAreaSet it identifies the first invalid polygon when available.
  // kOutside/kInvalidPoint/no-array failures use kNoGeofenceAreaIndex.
  uint16_t area_index;
};

// Pure union semantics for already-provided permitted polygons. The function
// validates every polygon before classifying the point so malformed later areas
// cannot be silently ignored merely because an earlier valid area contains the
// point. This slice deliberately does not freeze a product/configuration limit on
// the number of simultaneous areas; a bounded config owner is required before
// production runtime integration.
PermittedAreaAssessment assessPermittedGeofenceAreas(
    const GeofenceAreaSetView& area_set, const GeoPointE7& point);

}  // namespace orun_tlp
