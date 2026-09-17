#include "geofence_geometry.h"

#include <stddef.h>

namespace orun_tlp {
namespace {

constexpr int32_t kMinimumLatitudeE7 = -900000000;
constexpr int32_t kMaximumLatitudeE7 = 900000000;
constexpr int32_t kMinimumLongitudeE7 = -1800000000;
constexpr int32_t kMaximumLongitudeE7 = 1800000000;

bool samePoint(const GeoPointE7& a, const GeoPointE7& b) {
  return a.latitude_e7 == b.latitude_e7 &&
         a.longitude_e7 == b.longitude_e7;
}

bool coordinateInRange(const GeoPointE7& point) {
  // M6C1 is deliberately local planar geometry, not spherical/global geometry.
  // Exact poles and the +/-180-degree longitude seam have multiple equivalent
  // longitude representations that this planar model cannot classify
  // consistently. Reject those singular boundaries explicitly instead of
  // accepting a legal geographic coordinate into an unsupported geometry domain.
  return point.latitude_e7 > kMinimumLatitudeE7 &&
         point.latitude_e7 < kMaximumLatitudeE7 &&
         point.longitude_e7 > kMinimumLongitudeE7 &&
         point.longitude_e7 < kMaximumLongitudeE7;
}

uint16_t effectiveVertexCount(const GeofencePolygonView& polygon) {
  return effectiveGeofenceVertexCount(polygon);
}

// x is longitude and y is latitude. All validated polygon/query deltas used by
// this helper are bounded by kMaximumPolygonSpanE7, so each product and their
// difference remain safely inside signed 64-bit.
int64_t cross(const GeoPointE7& a, const GeoPointE7& b,
              const GeoPointE7& point) {
  const int64_t ab_x = static_cast<int64_t>(b.longitude_e7) - a.longitude_e7;
  const int64_t ab_y = static_cast<int64_t>(b.latitude_e7) - a.latitude_e7;
  const int64_t ap_x =
      static_cast<int64_t>(point.longitude_e7) - a.longitude_e7;
  const int64_t ap_y =
      static_cast<int64_t>(point.latitude_e7) - a.latitude_e7;
  return ab_x * ap_y - ab_y * ap_x;
}

int orientation(const GeoPointE7& a, const GeoPointE7& b,
                const GeoPointE7& point) {
  const int64_t value = cross(a, b, point);
  if (value < 0) return -1;
  if (value > 0) return 1;
  return 0;
}

bool betweenInclusive(int32_t value, int32_t a, int32_t b) {
  const int32_t low = a < b ? a : b;
  const int32_t high = a < b ? b : a;
  return value >= low && value <= high;
}

bool pointOnSegment(const GeoPointE7& a, const GeoPointE7& b,
                    const GeoPointE7& point) {
  return orientation(a, b, point) == 0 &&
         betweenInclusive(point.latitude_e7, a.latitude_e7, b.latitude_e7) &&
         betweenInclusive(point.longitude_e7, a.longitude_e7, b.longitude_e7);
}

bool segmentsIntersect(const GeoPointE7& a, const GeoPointE7& b,
                       const GeoPointE7& c, const GeoPointE7& d) {
  const int o1 = orientation(a, b, c);
  const int o2 = orientation(a, b, d);
  const int o3 = orientation(c, d, a);
  const int o4 = orientation(c, d, b);

  if (o1 != o2 && o3 != o4) return true;
  if (o1 == 0 && pointOnSegment(a, b, c)) return true;
  if (o2 == 0 && pointOnSegment(a, b, d)) return true;
  if (o3 == 0 && pointOnSegment(c, d, a)) return true;
  if (o4 == 0 && pointOnSegment(c, d, b)) return true;
  return false;
}

bool edgesAreAdjacent(uint16_t first, uint16_t second, uint16_t count) {
  if (first == second) return true;
  const uint16_t first_next = static_cast<uint16_t>((first + 1U) % count);
  const uint16_t second_next = static_cast<uint16_t>((second + 1U) % count);
  return first_next == second || second_next == first;
}

}  // namespace

GeofencePolygonValidation validateGeofencePolygon(
    const GeofencePolygonView& polygon) {
  if (polygon.vertices == nullptr)
    return GeofencePolygonValidation::kNullVertices;

  const uint16_t count = effectiveVertexCount(polygon);
  if (count < 3) return GeofencePolygonValidation::kTooFewVertices;
  if (count > geofence_config::kMaximumPolygonVertices)
    return GeofencePolygonValidation::kTooManyVertices;

  int32_t minimum_latitude = polygon.vertices[0].latitude_e7;
  int32_t maximum_latitude = polygon.vertices[0].latitude_e7;
  int32_t minimum_longitude = polygon.vertices[0].longitude_e7;
  int32_t maximum_longitude = polygon.vertices[0].longitude_e7;

  for (uint16_t i = 0; i < count; ++i) {
    const GeoPointE7& current = polygon.vertices[i];
    if (!coordinateInRange(current))
      return GeofencePolygonValidation::kCoordinateOutOfRange;

    const GeoPointE7& next = polygon.vertices[(i + 1U) % count];
    if (samePoint(current, next))
      return GeofencePolygonValidation::kDuplicateAdjacentVertex;

    if (current.latitude_e7 < minimum_latitude)
      minimum_latitude = current.latitude_e7;
    if (current.latitude_e7 > maximum_latitude)
      maximum_latitude = current.latitude_e7;
    if (current.longitude_e7 < minimum_longitude)
      minimum_longitude = current.longitude_e7;
    if (current.longitude_e7 > maximum_longitude)
      maximum_longitude = current.longitude_e7;
  }

  const int64_t latitude_span =
      static_cast<int64_t>(maximum_latitude) - minimum_latitude;
  const int64_t longitude_span =
      static_cast<int64_t>(maximum_longitude) - minimum_longitude;
  if (latitude_span > geofence_config::kMaximumPolygonSpanE7 ||
      longitude_span > geofence_config::kMaximumPolygonSpanE7) {
    return GeofencePolygonValidation::kSpanTooLarge;
  }

  // Translate around the first vertex so the shoelace sum uses only bounded
  // field-scale deltas rather than global E7 coordinates.
  const int64_t origin_x = polygon.vertices[0].longitude_e7;
  const int64_t origin_y = polygon.vertices[0].latitude_e7;
  int64_t signed_area_twice = 0;
  for (uint16_t i = 0; i < count; ++i) {
    const GeoPointE7& a = polygon.vertices[i];
    const GeoPointE7& b = polygon.vertices[(i + 1U) % count];
    const int64_t ax = static_cast<int64_t>(a.longitude_e7) - origin_x;
    const int64_t ay = static_cast<int64_t>(a.latitude_e7) - origin_y;
    const int64_t bx = static_cast<int64_t>(b.longitude_e7) - origin_x;
    const int64_t by = static_cast<int64_t>(b.latitude_e7) - origin_y;
    signed_area_twice += ax * by - ay * bx;
  }
  if (signed_area_twice == 0)
    return GeofencePolygonValidation::kDegenerate;

  // A self-intersecting polygon has ambiguous inside/outside semantics. Reject it
  // at the geometry boundary instead of relying on a fill rule accidentally.
  for (uint16_t first = 0; first < count; ++first) {
    const GeoPointE7& a = polygon.vertices[first];
    const GeoPointE7& b = polygon.vertices[(first + 1U) % count];
    for (uint16_t second = static_cast<uint16_t>(first + 1U);
         second < count; ++second) {
      if (edgesAreAdjacent(first, second, count)) continue;
      const GeoPointE7& c = polygon.vertices[second];
      const GeoPointE7& d = polygon.vertices[(second + 1U) % count];
      if (segmentsIntersect(a, b, c, d))
        return GeofencePolygonValidation::kSelfIntersecting;
    }
  }

  return GeofencePolygonValidation::kOk;
}

GeofencePointRelation classifyPointInGeofencePolygon(
    const GeofencePolygonView& polygon, const GeoPointE7& point) {
  if (validateGeofencePolygon(polygon) != GeofencePolygonValidation::kOk)
    return GeofencePointRelation::kInvalidPolygon;
  if (!coordinateInRange(point)) return GeofencePointRelation::kInvalidPoint;

  const uint16_t count = effectiveVertexCount(polygon);
  int32_t minimum_latitude = polygon.vertices[0].latitude_e7;
  int32_t maximum_latitude = polygon.vertices[0].latitude_e7;
  int32_t minimum_longitude = polygon.vertices[0].longitude_e7;
  int32_t maximum_longitude = polygon.vertices[0].longitude_e7;
  for (uint16_t i = 1; i < count; ++i) {
    const GeoPointE7& vertex = polygon.vertices[i];
    if (vertex.latitude_e7 < minimum_latitude)
      minimum_latitude = vertex.latitude_e7;
    if (vertex.latitude_e7 > maximum_latitude)
      maximum_latitude = vertex.latitude_e7;
    if (vertex.longitude_e7 < minimum_longitude)
      minimum_longitude = vertex.longitude_e7;
    if (vertex.longitude_e7 > maximum_longitude)
      maximum_longitude = vertex.longitude_e7;
  }

  if (point.latitude_e7 < minimum_latitude ||
      point.latitude_e7 > maximum_latitude ||
      point.longitude_e7 < minimum_longitude ||
      point.longitude_e7 > maximum_longitude) {
    return GeofencePointRelation::kOutside;
  }

  for (uint16_t i = 0; i < count; ++i) {
    const GeoPointE7& a = polygon.vertices[i];
    const GeoPointE7& b = polygon.vertices[(i + 1U) % count];
    if (pointOnSegment(a, b, point)) return GeofencePointRelation::kBoundary;
  }

  // Winding-number containment avoids division/floating point and therefore
  // remains deterministic on host and nRF52. Boundary has already been handled.
  int winding_number = 0;
  for (uint16_t i = 0; i < count; ++i) {
    const GeoPointE7& a = polygon.vertices[i];
    const GeoPointE7& b = polygon.vertices[(i + 1U) % count];
    if (a.latitude_e7 <= point.latitude_e7) {
      if (b.latitude_e7 > point.latitude_e7 && cross(a, b, point) > 0)
        ++winding_number;
    } else if (b.latitude_e7 <= point.latitude_e7 &&
               cross(a, b, point) < 0) {
      --winding_number;
    }
  }

  return winding_number == 0 ? GeofencePointRelation::kOutside
                             : GeofencePointRelation::kInside;
}

}  // namespace orun_tlp
