#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "geofence_geometry.h"

using namespace orun_tlp;

namespace {

GeofencePolygonView view(const GeoPointE7* vertices, uint16_t count) {
  return GeofencePolygonView(vertices, count);
}

void squareClassifiesInsideBoundaryAndOutside() {
  const GeoPointE7 square[] = {
      GeoPointE7(0, 0), GeoPointE7(0, 1000),
      GeoPointE7(1000, 1000), GeoPointE7(1000, 0)};
  const GeofencePolygonView polygon = view(square, 4);

  assert(validateGeofencePolygon(polygon) == GeofencePolygonValidation::kOk);
  assert(classifyPointInGeofencePolygon(polygon, GeoPointE7(500, 500)) ==
         GeofencePointRelation::kInside);
  assert(classifyPointInGeofencePolygon(polygon, GeoPointE7(0, 500)) ==
         GeofencePointRelation::kBoundary);
  assert(classifyPointInGeofencePolygon(polygon, GeoPointE7(1000, 1000)) ==
         GeofencePointRelation::kBoundary);
  assert(classifyPointInGeofencePolygon(polygon, GeoPointE7(1001, 500)) ==
         GeofencePointRelation::kOutside);
}

void windingDirectionDoesNotChangeContainment() {
  const GeoPointE7 clockwise[] = {
      GeoPointE7(0, 0), GeoPointE7(1000, 0),
      GeoPointE7(1000, 1000), GeoPointE7(0, 1000)};
  const GeofencePolygonView polygon = view(clockwise, 4);
  assert(validateGeofencePolygon(polygon) == GeofencePolygonValidation::kOk);
  assert(classifyPointInGeofencePolygon(polygon, GeoPointE7(500, 500)) ==
         GeofencePointRelation::kInside);
}

void concavePolygonDoesNotFillItsNotch() {
  const GeoPointE7 concave[] = {
      GeoPointE7(0, 0), GeoPointE7(0, 1000), GeoPointE7(400, 1000),
      GeoPointE7(400, 400), GeoPointE7(1000, 400), GeoPointE7(1000, 0)};
  const GeofencePolygonView polygon = view(concave, 6);
  assert(validateGeofencePolygon(polygon) == GeofencePolygonValidation::kOk);
  assert(classifyPointInGeofencePolygon(polygon, GeoPointE7(200, 800)) ==
         GeofencePointRelation::kInside);
  assert(classifyPointInGeofencePolygon(polygon, GeoPointE7(800, 200)) ==
         GeofencePointRelation::kInside);
  assert(classifyPointInGeofencePolygon(polygon, GeoPointE7(800, 800)) ==
         GeofencePointRelation::kOutside);
}

void explicitClosingVertexIsAccepted() {
  const GeoPointE7 closed[] = {
      GeoPointE7(0, 0), GeoPointE7(0, 1000), GeoPointE7(1000, 1000),
      GeoPointE7(1000, 0), GeoPointE7(0, 0)};
  const GeofencePolygonView polygon = view(closed, 5);
  assert(validateGeofencePolygon(polygon) == GeofencePolygonValidation::kOk);
  assert(classifyPointInGeofencePolygon(polygon, GeoPointE7(500, 500)) ==
         GeofencePointRelation::kInside);
}

void malformedPolygonsFailClosed() {
  assert(validateGeofencePolygon(GeofencePolygonView(nullptr, 0)) ==
         GeofencePolygonValidation::kNullVertices);

  const GeoPointE7 two[] = {GeoPointE7(0, 0), GeoPointE7(1, 1)};
  assert(validateGeofencePolygon(view(two, 2)) ==
         GeofencePolygonValidation::kTooFewVertices);

  // kMaximumPolygonVertices is an effective-vertex limit. A raw count of
  // max+1 can still be valid only when the last point explicitly closes the
  // polygon by repeating the first. Make this oversized fixture deterministic
  // and non-closing; leaving the stack array uninitialized made the test itself
  // undefined and could accidentally look explicitly closed.
  GeoPointE7 too_many[geofence_config::kMaximumPolygonVertices + 1] = {};
  too_many[geofence_config::kMaximumPolygonVertices] = GeoPointE7(1, 1);
  assert(validateGeofencePolygon(
             view(too_many, geofence_config::kMaximumPolygonVertices + 1)) ==
         GeofencePolygonValidation::kTooManyVertices);

  const GeoPointE7 invalid_coordinate[] = {
      GeoPointE7(900000001, 0), GeoPointE7(0, 1000), GeoPointE7(1000, 0)};
  assert(validateGeofencePolygon(view(invalid_coordinate, 3)) ==
         GeofencePolygonValidation::kCoordinateOutOfRange);

  const GeoPointE7 too_wide[] = {
      GeoPointE7(0, 0),
      GeoPointE7(0, geofence_config::kMaximumPolygonSpanE7 + 1),
      GeoPointE7(1000, geofence_config::kMaximumPolygonSpanE7 + 1),
      GeoPointE7(1000, 0)};
  assert(validateGeofencePolygon(view(too_wide, 4)) ==
         GeofencePolygonValidation::kSpanTooLarge);

  const GeoPointE7 duplicate_edge[] = {
      GeoPointE7(0, 0), GeoPointE7(0, 1000), GeoPointE7(0, 1000),
      GeoPointE7(1000, 0)};
  assert(validateGeofencePolygon(view(duplicate_edge, 4)) ==
         GeofencePolygonValidation::kDuplicateAdjacentVertex);

  const GeoPointE7 collinear[] = {
      GeoPointE7(0, 0), GeoPointE7(500, 500), GeoPointE7(1000, 1000)};
  assert(validateGeofencePolygon(view(collinear, 3)) ==
         GeofencePolygonValidation::kDegenerate);

  const GeoPointE7 self_crossing[] = {
      GeoPointE7(0, 0), GeoPointE7(1000, 800),
      GeoPointE7(0, 1000), GeoPointE7(800, 0)};
  assert(validateGeofencePolygon(view(self_crossing, 4)) ==
         GeofencePolygonValidation::kSelfIntersecting);

  assert(classifyPointInGeofencePolygon(view(collinear, 3), GeoPointE7(0, 0)) ==
         GeofencePointRelation::kInvalidPolygon);
}

void invalidQueryPointIsNotTreatedAsOutside() {
  const GeoPointE7 square[] = {
      GeoPointE7(0, 0), GeoPointE7(0, 1000),
      GeoPointE7(1000, 1000), GeoPointE7(1000, 0)};
  const GeofencePolygonView polygon = view(square, 4);
  assert(classifyPointInGeofencePolygon(polygon, GeoPointE7(900000001, 500)) ==
         GeofencePointRelation::kInvalidPoint);
}

void translatedFieldCoordinatesRemainDeterministic() {
  constexpr int32_t kBaseLatitude = 376000000;
  constexpr int32_t kBaseLongitude = 282000000;
  const GeoPointE7 field[] = {
      GeoPointE7(kBaseLatitude, kBaseLongitude),
      GeoPointE7(kBaseLatitude, kBaseLongitude + 20000),
      GeoPointE7(kBaseLatitude + 15000, kBaseLongitude + 20000),
      GeoPointE7(kBaseLatitude + 15000, kBaseLongitude)};
  const GeofencePolygonView polygon = view(field, 4);
  assert(validateGeofencePolygon(polygon) == GeofencePolygonValidation::kOk);
  assert(classifyPointInGeofencePolygon(
             polygon, GeoPointE7(kBaseLatitude + 5000, kBaseLongitude + 5000)) ==
         GeofencePointRelation::kInside);
}

void localPolygonNearCoordinateLimitRemainsSafe() {
  const GeoPointE7 polarish[] = {
      GeoPointE7(899990000, 1799990000), GeoPointE7(899990000, 1799995000),
      GeoPointE7(899995000, 1799995000), GeoPointE7(899995000, 1799990000)};
  const GeofencePolygonView polygon = view(polarish, 4);
  assert(validateGeofencePolygon(polygon) == GeofencePolygonValidation::kOk);
  assert(classifyPointInGeofencePolygon(
             polygon, GeoPointE7(899992000, 1799992000)) ==
         GeofencePointRelation::kInside);
}

}  // namespace

int main() {
  squareClassifiesInsideBoundaryAndOutside();
  windingDirectionDoesNotChangeContainment();
  concavePolygonDoesNotFillItsNotch();
  explicitClosingVertexIsAccepted();
  malformedPolygonsFailClosed();
  invalidQueryPointIsNotTreatedAsOutside();
  translatedFieldCoordinatesRemainDeterministic();
  localPolygonNearCoordinateLimitRemainsSafe();
  puts("M6C bounded geofence polygon geometry checks: PASS");
}
