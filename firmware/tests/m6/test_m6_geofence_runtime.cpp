#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "geofence_runtime.h"

using namespace orun_tlp;

namespace {

GeofencePolygonView polygon(const GeoPointE7* vertices, uint16_t count) {
  return GeofencePolygonView(vertices, count);
}

GnssFix fixAt(int32_t latitude_e7, int32_t longitude_e7,
              uint32_t captured_at_ms) {
  GnssFix fix{};
  fix.latitude_e7 = latitude_e7;
  fix.longitude_e7 = longitude_e7;
  fix.captured_at_ms = captured_at_ms;
  return fix;
}

void unconfiguredRuntimeDoesNotInventState() {
  GeofenceRuntime runtime;
  assert(!runtime.configured());
  assert(!runtime.hasAssessment());
  assert(runtime.observeAcceptedFix(fixAt(0, 0, 10)) ==
         GeofenceObservationResult::kNotConfigured);
  assert(!runtime.hasAssessment());
}

void boundedOwnedCopyAndUnionAssessment() {
  GeoPointE7 first[] = {GeoPointE7(0, 0), GeoPointE7(0, 1000),
                        GeoPointE7(1000, 1000), GeoPointE7(1000, 0)};
  GeoPointE7 second[] = {GeoPointE7(5000, 5000), GeoPointE7(5000, 6000),
                         GeoPointE7(6000, 6000), GeoPointE7(6000, 5000)};
  const GeofencePolygonView areas[] = {polygon(first, 4), polygon(second, 4)};

  GeofenceRuntime runtime;
  assert(runtime.configure(GeofenceAreaSetView(areas, 2)) ==
         GeofenceRuntimeConfigResult::kApplied);
  assert(runtime.configured());
  assert(runtime.areaCount() == 2);
  assert(runtime.totalVertexCount() == 8);

  // Runtime owns its copy: mutating the caller buffer after configure must not
  // alter the active geofence.
  first[0] = GeoPointE7(700000000, 700000000);

  assert(runtime.observeAcceptedFix(fixAt(5500, 5500, 100)) ==
         GeofenceObservationResult::kAccepted);
  assert(runtime.assessment().relation == PermittedAreaRelation::kInside);
  assert(runtime.assessment().area_index == 1);
  assert(runtime.assessedFixCapturedAtMs() == 100);

  assert(runtime.observeAcceptedFix(fixAt(0, 500, 200)) ==
         GeofenceObservationResult::kAccepted);
  assert(runtime.assessment().relation == PermittedAreaRelation::kBoundary);
  assert(runtime.assessment().area_index == 0);

  assert(runtime.observeAcceptedFix(fixAt(3000, 3000, 300)) ==
         GeofenceObservationResult::kAccepted);
  assert(runtime.assessment().relation == PermittedAreaRelation::kOutside);
  assert(runtime.assessment().area_index == kNoGeofenceAreaIndex);
}

void invalidObservationDoesNotEraseLastValidAssessment() {
  const GeoPointE7 vertices[] = {
      GeoPointE7(0, 0), GeoPointE7(0, 1000),
      GeoPointE7(1000, 1000), GeoPointE7(1000, 0)};
  const GeofencePolygonView areas[] = {polygon(vertices, 4)};
  GeofenceRuntime runtime;
  assert(runtime.configure(GeofenceAreaSetView(areas, 1)) ==
         GeofenceRuntimeConfigResult::kApplied);
  assert(runtime.observeAcceptedFix(fixAt(500, 500, 77)) ==
         GeofenceObservationResult::kAccepted);
  const PermittedAreaAssessment before = runtime.assessment();

  assert(runtime.observeAcceptedFix(fixAt(900000001, 0, 88)) ==
         GeofenceObservationResult::kInvalidPoint);
  assert(runtime.hasAssessment());
  assert(runtime.assessment().relation == before.relation);
  assert(runtime.assessment().area_index == before.area_index);
  assert(runtime.assessedFixCapturedAtMs() == 77);
}

void rejectedReplacementPreservesKnownGoodConfigAndResult() {
  const GeoPointE7 valid[] = {
      GeoPointE7(0, 0), GeoPointE7(0, 1000),
      GeoPointE7(1000, 1000), GeoPointE7(1000, 0)};
  const GeofencePolygonView valid_areas[] = {polygon(valid, 4)};
  GeofenceRuntime runtime;
  assert(runtime.configure(GeofenceAreaSetView(valid_areas, 1)) ==
         GeofenceRuntimeConfigResult::kApplied);
  assert(runtime.observeAcceptedFix(fixAt(500, 500, 9)) ==
         GeofenceObservationResult::kAccepted);

  const GeoPointE7 malformed[] = {
      GeoPointE7(0, 0), GeoPointE7(500, 500), GeoPointE7(1000, 1000)};
  const GeofencePolygonView invalid_areas[] = {polygon(malformed, 3)};
  assert(runtime.configure(GeofenceAreaSetView(invalid_areas, 1)) ==
         GeofenceRuntimeConfigResult::kInvalidAreaSet);
  assert(runtime.configured());
  assert(runtime.areaCount() == 1);
  assert(runtime.totalVertexCount() == 4);
  assert(runtime.hasAssessment());
  assert(runtime.assessment().relation == PermittedAreaRelation::kInside);
  assert(runtime.assessedFixCapturedAtMs() == 9);
}

void resourceBoundsRejectWithoutMutatingActiveConfig() {
  const GeoPointE7 valid[] = {
      GeoPointE7(0, 0), GeoPointE7(0, 1000),
      GeoPointE7(1000, 1000), GeoPointE7(1000, 0)};
  const GeofencePolygonView one[] = {polygon(valid, 4)};
  GeofenceRuntime runtime;
  assert(runtime.configure(GeofenceAreaSetView(one, 1)) ==
         GeofenceRuntimeConfigResult::kApplied);

  GeofencePolygonView too_many_areas[
      geofence_runtime_config::kMaximumAreas + 1]{};
  assert(runtime.configure(GeofenceAreaSetView(
             too_many_areas, geofence_runtime_config::kMaximumAreas + 1)) ==
         GeofenceRuntimeConfigResult::kTooManyAreas);
  assert(runtime.areaCount() == 1 && runtime.totalVertexCount() == 4);

  GeoPointE7 too_many_vertices[
      geofence_runtime_config::kMaximumTotalVertices + 1]{};
  const GeofencePolygonView oversized[] = {
      polygon(too_many_vertices,
              geofence_runtime_config::kMaximumTotalVertices + 1)};
  assert(runtime.configure(GeofenceAreaSetView(oversized, 1)) ==
         GeofenceRuntimeConfigResult::kTooManyVertices);
  assert(runtime.areaCount() == 1 && runtime.totalVertexCount() == 4);
}

void successfulReplacementAndExplicitClearResetAssessment() {
  const GeoPointE7 first[] = {
      GeoPointE7(0, 0), GeoPointE7(0, 1000),
      GeoPointE7(1000, 1000), GeoPointE7(1000, 0)};
  const GeofencePolygonView first_area[] = {polygon(first, 4)};
  GeofenceRuntime runtime;
  assert(runtime.configure(GeofenceAreaSetView(first_area, 1)) ==
         GeofenceRuntimeConfigResult::kApplied);
  assert(runtime.observeAcceptedFix(fixAt(500, 500, UINT32_MAX - 3)) ==
         GeofenceObservationResult::kAccepted);
  assert(runtime.hasAssessment());

  const GeoPointE7 second[] = {
      GeoPointE7(5000, 5000), GeoPointE7(5000, 6000),
      GeoPointE7(6000, 6000), GeoPointE7(6000, 5000)};
  const GeofencePolygonView second_area[] = {polygon(second, 4)};
  assert(runtime.configure(GeofenceAreaSetView(second_area, 1)) ==
         GeofenceRuntimeConfigResult::kApplied);
  assert(!runtime.hasAssessment());
  assert(runtime.observeAcceptedFix(fixAt(5500, 5500, 2)) ==
         GeofenceObservationResult::kAccepted);
  assert(runtime.assessedFixCapturedAtMs() == 2);

  runtime.clear();
  assert(!runtime.configured());
  assert(runtime.areaCount() == 0);
  assert(runtime.totalVertexCount() == 0);
  assert(!runtime.hasAssessment());
  assert(runtime.observeAcceptedFix(fixAt(5500, 5500, 3)) ==
         GeofenceObservationResult::kNotConfigured);
}

}  // namespace

int main() {
  unconfiguredRuntimeDoesNotInventState();
  boundedOwnedCopyAndUnionAssessment();
  invalidObservationDoesNotEraseLastValidAssessment();
  rejectedReplacementPreservesKnownGoodConfigAndResult();
  resourceBoundsRejectWithoutMutatingActiveConfig();
  successfulReplacementAndExplicitClearResetAssessment();
  puts("M6C3 bounded geofence runtime checks: PASS");
}
