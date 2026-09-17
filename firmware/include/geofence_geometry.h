#pragma once

#include <stdint.h>

namespace orun_tlp {
namespace geofence_config {

// M6C1 deliberately supports field-scale polygons only. The span bound keeps
// all orientation/area arithmetic provably inside signed 64-bit while remaining
// far larger than the intended livestock geofence scale. Antimeridian/global
// polygons are a separate future requirement, not something to approximate
// silently. Exact +/-180-degree longitude and +/-90-degree latitude singular
// boundaries are therefore outside this local planar geometry domain even though
// they are legal geographic coordinates.
constexpr uint16_t kMaximumPolygonVertices = 64;
constexpr int32_t kMaximumPolygonSpanE7 = 100000000;  // 10 degrees.

}  // namespace geofence_config

struct GeoPointE7 {
  constexpr GeoPointE7(int32_t latitude_value = 0,
                       int32_t longitude_value = 0)
      : latitude_e7(latitude_value), longitude_e7(longitude_value) {}

  int32_t latitude_e7;
  int32_t longitude_e7;
};

struct GeofencePolygonView {
  constexpr GeofencePolygonView(const GeoPointE7* vertices_value = nullptr,
                                uint16_t vertex_count_value = 0)
      : vertices(vertices_value), vertex_count(vertex_count_value) {}

  const GeoPointE7* vertices;
  uint16_t vertex_count;
};

// Structural count shared by geometry validation/classification and bounded
// runtime ownership. An optional explicit final vertex equal to the first is a
// closing duplicate and therefore does not consume an effective-vertex slot.
// Null/empty views have zero effective vertices.
inline uint16_t effectiveGeofenceVertexCount(
    const GeofencePolygonView& polygon) {
  if (polygon.vertices == nullptr || polygon.vertex_count == 0) return 0;
  if (polygon.vertex_count > 1 &&
      polygon.vertices[0].latitude_e7 ==
          polygon.vertices[polygon.vertex_count - 1].latitude_e7 &&
      polygon.vertices[0].longitude_e7 ==
          polygon.vertices[polygon.vertex_count - 1].longitude_e7) {
    return static_cast<uint16_t>(polygon.vertex_count - 1);
  }
  return polygon.vertex_count;
}

enum class GeofencePolygonValidation : uint8_t {
  kOk,
  kNullVertices,
  kTooFewVertices,
  kTooManyVertices,
  kCoordinateOutOfRange,
  kSpanTooLarge,
  kDuplicateAdjacentVertex,
  kDegenerate,
  kSelfIntersecting,
};

enum class GeofencePointRelation : uint8_t {
  kInside,
  kBoundary,
  kOutside,
  kInvalidPolygon,
  kInvalidPoint,
};

// Validates a simple field-scale polygon without allocation. Both an implicit
// closing edge and an explicit final vertex equal to the first are accepted.
GeofencePolygonValidation validateGeofencePolygon(
    const GeofencePolygonView& polygon);

// Pure geometry only. This does not own GNSS quality/freshness, hysteresis,
// FREE_GRAZE, service enablement or OUTSIDE/LOST operational policy. Query points
// at the exact global seam/poles are rejected as kInvalidPoint because M6C1 does
// not implement spherical-equivalence handling.
GeofencePointRelation classifyPointInGeofencePolygon(
    const GeofencePolygonView& polygon, const GeoPointE7& point);

}  // namespace orun_tlp
