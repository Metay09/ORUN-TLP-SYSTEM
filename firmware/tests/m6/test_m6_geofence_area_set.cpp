#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "geofence_area_set.h"

using namespace orun_tlp;

namespace {

GeofencePolygonView polygon(const GeoPointE7* vertices, uint16_t count) {
  return GeofencePolygonView(vertices, count);
}

void nullAndEmptySetsFailClosed() {
  const GeoPointE7 point(0, 0);
  PermittedAreaAssessment assessment =
      assessPermittedGeofenceAreas(GeofenceAreaSetView(nullptr, 0), point);
  assert(assessment.relation == PermittedAreaRelation::kInvalidAreaSet);
  assert(assessment.area_index == kNoGeofenceAreaIndex);

  GeofencePolygonView dummy;
  assessment = assessPermittedGeofenceAreas(GeofenceAreaSetView(&dummy, 0), point);
  assert(assessment.relation == PermittedAreaRelation::kInvalidAreaSet);
}

void pointInsideSecondPermittedAreaIsInsideUnion() {
  const GeoPointE7 first_vertices[] = {
      GeoPointE7(0, 0), GeoPointE7(0, 1000),
      GeoPointE7(1000, 1000), GeoPointE7(1000, 0)};
  const GeoPointE7 second_vertices[] = {
      GeoPointE7(5000, 5000), GeoPointE7(5000, 6000),
      GeoPointE7(6000, 6000), GeoPointE7(6000, 5000)};
  const GeofencePolygonView areas[] = {
      polygon(first_vertices, 4), polygon(second_vertices, 4)};

  const PermittedAreaAssessment assessment = assessPermittedGeofenceAreas(
      GeofenceAreaSetView(areas, 2), GeoPointE7(5500, 5500));
  assert(assessment.relation == PermittedAreaRelation::kInside);
  assert(assessment.area_index == 1);
}

void boundaryIsReportedWhenNoAreaContainsPoint() {
  const GeoPointE7 first_vertices[] = {
      GeoPointE7(0, 0), GeoPointE7(0, 1000),
      GeoPointE7(1000, 1000), GeoPointE7(1000, 0)};
  const GeoPointE7 second_vertices[] = {
      GeoPointE7(5000, 5000), GeoPointE7(5000, 6000),
      GeoPointE7(6000, 6000), GeoPointE7(6000, 5000)};
  const GeofencePolygonView areas[] = {
      polygon(first_vertices, 4), polygon(second_vertices, 4)};

  const PermittedAreaAssessment assessment = assessPermittedGeofenceAreas(
      GeofenceAreaSetView(areas, 2), GeoPointE7(0, 500));
  assert(assessment.relation == PermittedAreaRelation::kBoundary);
  assert(assessment.area_index == 0);
}

void insideOutranksBoundaryForOverlappingPermittedUnion() {
  const GeoPointE7 first_vertices[] = {
      GeoPointE7(0, 0), GeoPointE7(0, 1000),
      GeoPointE7(1000, 1000), GeoPointE7(1000, 0)};
  const GeoPointE7 second_vertices[] = {
      GeoPointE7(-500, 500), GeoPointE7(-500, 1500),
      GeoPointE7(500, 1500), GeoPointE7(500, 500)};
  const GeofencePolygonView areas[] = {
      polygon(first_vertices, 4), polygon(second_vertices, 4)};

  // (500,500) is inside the first square and exactly on the second square's
  // lower-right vertex. For the union of permitted areas it is interior because
  // at least one permitted polygon contains it.
  const PermittedAreaAssessment assessment = assessPermittedGeofenceAreas(
      GeofenceAreaSetView(areas, 2), GeoPointE7(500, 500));
  assert(assessment.relation == PermittedAreaRelation::kInside);
  assert(assessment.area_index == 0);
}

void outsideAllAreasRemainsOutside() {
  const GeoPointE7 first_vertices[] = {
      GeoPointE7(0, 0), GeoPointE7(0, 1000),
      GeoPointE7(1000, 1000), GeoPointE7(1000, 0)};
  const GeoPointE7 second_vertices[] = {
      GeoPointE7(5000, 5000), GeoPointE7(5000, 6000),
      GeoPointE7(6000, 6000), GeoPointE7(6000, 5000)};
  const GeofencePolygonView areas[] = {
      polygon(first_vertices, 4), polygon(second_vertices, 4)};

  const PermittedAreaAssessment assessment = assessPermittedGeofenceAreas(
      GeofenceAreaSetView(areas, 2), GeoPointE7(3000, 3000));
  assert(assessment.relation == PermittedAreaRelation::kOutside);
  assert(assessment.area_index == kNoGeofenceAreaIndex);
}

void malformedLaterAreaInvalidatesWholeSetBeforeContainment() {
  const GeoPointE7 valid_vertices[] = {
      GeoPointE7(0, 0), GeoPointE7(0, 1000),
      GeoPointE7(1000, 1000), GeoPointE7(1000, 0)};
  const GeoPointE7 invalid_vertices[] = {
      GeoPointE7(5000, 5000), GeoPointE7(5500, 5500),
      GeoPointE7(6000, 6000)};
  const GeofencePolygonView areas[] = {
      polygon(valid_vertices, 4), polygon(invalid_vertices, 3)};

  // The point is inside area 0, but a malformed configured area must fail the
  // whole set closed instead of making behavior depend on polygon ordering.
  const PermittedAreaAssessment assessment = assessPermittedGeofenceAreas(
      GeofenceAreaSetView(areas, 2), GeoPointE7(500, 500));
  assert(assessment.relation == PermittedAreaRelation::kInvalidAreaSet);
  assert(assessment.area_index == 1);
}

void invalidPointIsDistinctFromOutside() {
  const GeoPointE7 vertices[] = {
      GeoPointE7(0, 0), GeoPointE7(0, 1000),
      GeoPointE7(1000, 1000), GeoPointE7(1000, 0)};
  const GeofencePolygonView areas[] = {polygon(vertices, 4)};

  const PermittedAreaAssessment assessment = assessPermittedGeofenceAreas(
      GeofenceAreaSetView(areas, 1), GeoPointE7(900000001, 0));
  assert(assessment.relation == PermittedAreaRelation::kInvalidPoint);
  assert(assessment.area_index == kNoGeofenceAreaIndex);
}

void explicitClosingVertexWorksInsideAreaSet() {
  const GeoPointE7 vertices[] = {
      GeoPointE7(0, 0), GeoPointE7(0, 1000), GeoPointE7(1000, 1000),
      GeoPointE7(1000, 0), GeoPointE7(0, 0)};
  const GeofencePolygonView areas[] = {polygon(vertices, 5)};

  const PermittedAreaAssessment assessment = assessPermittedGeofenceAreas(
      GeofenceAreaSetView(areas, 1), GeoPointE7(500, 500));
  assert(assessment.relation == PermittedAreaRelation::kInside);
  assert(assessment.area_index == 0);
}

}  // namespace

int main() {
  nullAndEmptySetsFailClosed();
  pointInsideSecondPermittedAreaIsInsideUnion();
  boundaryIsReportedWhenNoAreaContainsPoint();
  insideOutranksBoundaryForOverlappingPermittedUnion();
  outsideAllAreasRemainsOutside();
  malformedLaterAreaInvalidatesWholeSetBeforeContainment();
  invalidPointIsDistinctFromOutside();
  explicitClosingVertexWorksInsideAreaSet();
  puts("M6C permitted geofence area-set checks: PASS");
}
