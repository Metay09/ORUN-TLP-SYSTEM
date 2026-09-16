#include "geofence_runtime.h"

namespace orun_tlp {
namespace {

bool samePoint(const GeoPointE7& a, const GeoPointE7& b) {
  return a.latitude_e7 == b.latitude_e7 &&
         a.longitude_e7 == b.longitude_e7;
}

uint16_t canonicalVertexCount(const GeofencePolygonView& polygon) {
  if (polygon.vertex_count > 1 &&
      samePoint(polygon.vertices[0],
                polygon.vertices[polygon.vertex_count - 1])) {
    return static_cast<uint16_t>(polygon.vertex_count - 1);
  }
  return polygon.vertex_count;
}

}  // namespace

GeofenceRuntimeConfigResult GeofenceRuntime::configure(
    const GeofenceAreaSetView& candidate) {
  if (candidate.areas == nullptr || candidate.area_count == 0) {
    return GeofenceRuntimeConfigResult::kInvalidAreaSet;
  }
  if (candidate.area_count > geofence_runtime_config::kMaximumAreas) {
    return GeofenceRuntimeConfigResult::kTooManyAreas;
  }

  // Validate and size the complete caller-owned candidate before touching active
  // state. A failed replacement therefore cannot partially corrupt the last
  // known-good runtime configuration or assessment.
  uint16_t total_vertices = 0;
  for (uint16_t i = 0; i < candidate.area_count; ++i) {
    const GeofencePolygonView& area = candidate.areas[i];
    const GeofencePolygonValidation validation = validateGeofencePolygon(area);
    if (validation == GeofencePolygonValidation::kTooManyVertices) {
      return GeofenceRuntimeConfigResult::kTooManyVertices;
    }
    if (validation != GeofencePolygonValidation::kOk) {
      return GeofenceRuntimeConfigResult::kInvalidAreaSet;
    }

    const uint16_t stored_vertices = canonicalVertexCount(area);
    if (stored_vertices >
        geofence_runtime_config::kMaximumTotalVertices - total_vertices) {
      return GeofenceRuntimeConfigResult::kTooManyVertices;
    }
    total_vertices = static_cast<uint16_t>(total_vertices + stored_vertices);
  }

  uint16_t vertex_offset = 0;
  for (uint16_t i = 0; i < candidate.area_count; ++i) {
    const GeofencePolygonView& source = candidate.areas[i];
    const uint16_t stored_vertices = canonicalVertexCount(source);
    const uint16_t start = vertex_offset;
    for (uint16_t vertex = 0; vertex < stored_vertices; ++vertex) {
      vertices_[vertex_offset++] = source.vertices[vertex];
    }
    areas_[i] = GeofencePolygonView(&vertices_[start], stored_vertices);
  }
  for (uint16_t i = candidate.area_count;
       i < geofence_runtime_config::kMaximumAreas; ++i) {
    areas_[i] = GeofencePolygonView();
  }

  area_count_ = static_cast<uint8_t>(candidate.area_count);
  total_vertex_count_ = total_vertices;
  configured_ = true;
  has_assessment_ = false;
  assessment_ = PermittedAreaAssessment();
  assessed_fix_captured_at_ms_ = 0;
  return GeofenceRuntimeConfigResult::kApplied;
}

void GeofenceRuntime::clear() {
  area_count_ = 0;
  total_vertex_count_ = 0;
  configured_ = false;
  has_assessment_ = false;
  assessment_ = PermittedAreaAssessment();
  assessed_fix_captured_at_ms_ = 0;
}

GeofenceObservationResult GeofenceRuntime::observeAcceptedFix(
    const GnssFix& fix) {
  if (!configured_) return GeofenceObservationResult::kNotConfigured;

  const PermittedAreaAssessment next = assessPermittedGeofenceAreas(
      GeofenceAreaSetView(areas_, area_count_),
      GeoPointE7(fix.latitude_e7, fix.longitude_e7));
  if (next.relation == PermittedAreaRelation::kInvalidPoint) {
    return GeofenceObservationResult::kInvalidPoint;
  }
  if (next.relation == PermittedAreaRelation::kInvalidAreaSet) {
    // Defensive only: configure() accepted and copied the complete set.
    return GeofenceObservationResult::kConfigFault;
  }

  assessment_ = next;
  assessed_fix_captured_at_ms_ = fix.captured_at_ms;
  has_assessment_ = true;
  return GeofenceObservationResult::kAccepted;
}

}  // namespace orun_tlp
