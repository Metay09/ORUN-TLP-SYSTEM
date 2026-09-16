#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "geofence_geometry.h"

using namespace orun_tlp;

namespace {

GeofencePolygonView view(const GeoPointE7* vertices, uint16_t count) {
  return GeofencePolygonView(vertices, count);
}

void exactAntimeridianVerticesFailClosed() {
  const GeoPointE7 plus_seam[] = {
      GeoPointE7(1000, 1800000000), GeoPointE7(1000, 1799999000),
      GeoPointE7(2000, 1799999000)};
  const GeoPointE7 minus_seam[] = {
      GeoPointE7(1000, -1800000000), GeoPointE7(1000, -1799999000),
      GeoPointE7(2000, -1799999000)};
  assert(validateGeofencePolygon(view(plus_seam, 3)) ==
         GeofencePolygonValidation::kCoordinateOutOfRange);
  assert(validateGeofencePolygon(view(minus_seam, 3)) ==
         GeofencePolygonValidation::kCoordinateOutOfRange);
}

void exactPoleVerticesFailClosed() {
  const GeoPointE7 north_pole[] = {
      GeoPointE7(900000000, 1000), GeoPointE7(899999000, 0),
      GeoPointE7(899999000, 2000)};
  const GeoPointE7 south_pole[] = {
      GeoPointE7(-900000000, 1000), GeoPointE7(-899999000, 0),
      GeoPointE7(-899999000, 2000)};
  assert(validateGeofencePolygon(view(north_pole, 3)) ==
         GeofencePolygonValidation::kCoordinateOutOfRange);
  assert(validateGeofencePolygon(view(south_pole, 3)) ==
         GeofencePolygonValidation::kCoordinateOutOfRange);
}

void exactSingularQueryPointsAreInvalidNotOutside() {
  const GeoPointE7 square[] = {
      GeoPointE7(0, 0), GeoPointE7(0, 1000),
      GeoPointE7(1000, 1000), GeoPointE7(1000, 0)};
  const GeofencePolygonView polygon = view(square, 4);
  assert(classifyPointInGeofencePolygon(
             polygon, GeoPointE7(0, 1800000000)) ==
         GeofencePointRelation::kInvalidPoint);
  assert(classifyPointInGeofencePolygon(
             polygon, GeoPointE7(0, -1800000000)) ==
         GeofencePointRelation::kInvalidPoint);
  assert(classifyPointInGeofencePolygon(
             polygon, GeoPointE7(900000000, 0)) ==
         GeofencePointRelation::kInvalidPoint);
  assert(classifyPointInGeofencePolygon(
             polygon, GeoPointE7(-900000000, 0)) ==
         GeofencePointRelation::kInvalidPoint);
}

void immediatelyInteriorCoordinateLimitsRemainSupported() {
  const GeoPointE7 near_northeast[] = {
      GeoPointE7(899998000, 1799998000),
      GeoPointE7(899998000, 1799999999),
      GeoPointE7(899999999, 1799999999),
      GeoPointE7(899999999, 1799998000)};
  const GeofencePolygonView polygon = view(near_northeast, 4);
  assert(validateGeofencePolygon(polygon) == GeofencePolygonValidation::kOk);
  assert(classifyPointInGeofencePolygon(
             polygon, GeoPointE7(899999000, 1799999000)) ==
         GeofencePointRelation::kInside);
}

}  // namespace

int main() {
  exactAntimeridianVerticesFailClosed();
  exactPoleVerticesFailClosed();
  exactSingularQueryPointsAreInvalidNotOutside();
  immediatelyInteriorCoordinateLimitsRemainSupported();
  puts("M6C unsupported global-domain rejection checks: PASS");
}
