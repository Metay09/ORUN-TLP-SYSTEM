#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <type_traits>

#include "geofence_runtime.h"

using namespace orun_tlp;

namespace {

static_assert(!std::is_copy_constructible<GeofenceRuntime>::value,
              "GeofenceRuntime owns self-referential views and must not copy");
static_assert(!std::is_copy_assignable<GeofenceRuntime>::value,
              "GeofenceRuntime owns self-referential views and must not assign");
static_assert(!std::is_move_constructible<GeofenceRuntime>::value,
              "GeofenceRuntime owns self-referential views and must not move");
static_assert(!std::is_move_assignable<GeofenceRuntime>::value,
              "GeofenceRuntime owns self-referential views and must not move-assign");

GeofencePolygonView polygon(const GeoPointE7* vertices, uint16_t count) {
  return GeofencePolygonView(vertices, count);
}

GeofenceObservationResult observeAt(GeofenceRuntime& runtime,
                                    int32_t latitude_e7,
                                    int32_t longitude_e7,
                                    uint32_t captured_at_ms) {
  return runtime.observeAcceptedPosition(
      GeoPointE7(latitude_e7, longitude_e7), captured_at_ms);
}

void makeMaximumClosedPolygon(GeoPointE7 (&vertices)[65]) {
  uint16_t out = 0;
  for (int32_t i = 0; i < 16; ++i) vertices[out++] = GeoPointE7(0, i);
  for (int32_t i = 0; i < 16; ++i) vertices[out++] = GeoPointE7(i, 16);
  for (int32_t i = 0; i < 16; ++i) vertices[out++] = GeoPointE7(16, 16 - i);
  for (int32_t i = 0; i < 16; ++i) vertices[out++] = GeoPointE7(16 - i, 0);
  assert(out == 64);
  vertices[64] = vertices[0];
}

void unconfiguredRuntimeDoesNotInventState() {
  GeofenceRuntime runtime;
  assert(!runtime.configured());
  assert(!runtime.hasAssessment());
  assert(observeAt(runtime, 0, 0, 10) ==
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

  // Runtime owns its copy: caller lifetime/mutation cannot alter active geometry.
  first[0] = GeoPointE7(700000000, 700000000);

  assert(observeAt(runtime, 5500, 5500, 100) ==
         GeofenceObservationResult::kAccepted);
  assert(runtime.assessment().relation == PermittedAreaRelation::kInside);
  assert(runtime.assessment().area_index == 1);
  assert(runtime.assessedObservationCapturedAtMs() == 100);

  assert(observeAt(runtime, 0, 500, 200) ==
         GeofenceObservationResult::kAccepted);
  assert(runtime.assessment().relation == PermittedAreaRelation::kBoundary);
  assert(runtime.assessment().area_index == 0);

  assert(observeAt(runtime, 3000, 3000, 300) ==
         GeofenceObservationResult::kAccepted);
  assert(runtime.assessment().relation == PermittedAreaRelation::kOutside);
  assert(runtime.assessment().area_index == kNoGeofenceAreaIndex);
}

void explicitClosingVertexUsesEffectiveVertexBudget() {
  GeoPointE7 maximum_closed[65]{};
  makeMaximumClosedPolygon(maximum_closed);
  const GeofencePolygonView areas[] = {polygon(maximum_closed, 65)};

  assert(validateGeofencePolygon(areas[0]) == GeofencePolygonValidation::kOk);

  GeofenceRuntime runtime;
  assert(runtime.configure(GeofenceAreaSetView(areas, 1)) ==
         GeofenceRuntimeConfigResult::kApplied);
  assert(runtime.totalVertexCount() == 64);
  assert(observeAt(runtime, 8, 8, 1234) ==
         GeofenceObservationResult::kAccepted);
  assert(runtime.assessment().relation == PermittedAreaRelation::kInside);
}

void invalidObservationDoesNotEraseLastValidAssessment() {
  const GeoPointE7 vertices[] = {
      GeoPointE7(0, 0), GeoPointE7(0, 1000),
      GeoPointE7(1000, 1000), GeoPointE7(1000, 0)};
  const GeofencePolygonView areas[] = {polygon(vertices, 4)};
  GeofenceRuntime runtime;
  assert(runtime.configure(GeofenceAreaSetView(areas, 1)) ==
         GeofenceRuntimeConfigResult::kApplied);
  assert(observeAt(runtime, 500, 500, 77) ==
         GeofenceObservationResult::kAccepted);
  const PermittedAreaAssessment before = runtime.assessment();

  assert(observeAt(runtime, 900000000, 0, 88) ==
         GeofenceObservationResult::kInvalidPoint);
  assert(runtime.hasAssessment());
  assert(runtime.assessment().relation == before.relation);
  assert(runtime.assessment().area_index == before.area_index);
  assert(runtime.assessedObservationCapturedAtMs() == 77);
}

void rejectedReplacementPreservesKnownGoodConfigAndResult() {
  const GeoPointE7 valid[] = {
      GeoPointE7(0, 0), GeoPointE7(0, 1000),
      GeoPointE7(1000, 1000), GeoPointE7(1000, 0)};
  const GeofencePolygonView valid_areas[] = {polygon(valid, 4)};
  GeofenceRuntime runtime;
  assert(runtime.configure(GeofenceAreaSetView(valid_areas, 1)) ==
         GeofenceRuntimeConfigResult::kApplied);
  assert(observeAt(runtime, 500, 500, 9) ==
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
  assert(runtime.assessedObservationCapturedAtMs() == 9);
}

void resourceBoundsRejectWithoutMutatingActiveConfig() {
  const GeoPointE7 valid[] = {
      GeoPointE7(0, 0), GeoPointE7(0, 1000),
      GeoPointE7(1000, 1000), GeoPointE7(1000, 0)};
  const GeofencePolygonView one[] = {polygon(valid, 4)};
  GeofenceRuntime runtime;
  assert(runtime.configure(GeofenceAreaSetView(one, 1)) ==
         GeofenceRuntimeConfigResult::kApplied);
  assert(observeAt(runtime, 500, 500, 55) ==
         GeofenceObservationResult::kAccepted);

  GeofencePolygonView too_many_areas[
      geofence_runtime_config::kMaximumAreas + 1]{};
  assert(runtime.configure(GeofenceAreaSetView(
             too_many_areas, geofence_runtime_config::kMaximumAreas + 1)) ==
         GeofenceRuntimeConfigResult::kTooManyAreas);
  assert(runtime.areaCount() == 1 && runtime.totalVertexCount() == 4);
  assert(runtime.hasAssessment());
  assert(runtime.assessment().relation == PermittedAreaRelation::kInside);
  assert(runtime.assessedObservationCapturedAtMs() == 55);

  GeoPointE7 maximum_closed[65]{};
  makeMaximumClosedPolygon(maximum_closed);
  const GeofencePolygonView oversized_total[] = {
      polygon(maximum_closed, 65), polygon(valid, 4)};
  assert(runtime.configure(GeofenceAreaSetView(oversized_total, 2)) ==
         GeofenceRuntimeConfigResult::kTooManyVertices);
  assert(runtime.areaCount() == 1 && runtime.totalVertexCount() == 4);
  assert(runtime.hasAssessment());
  assert(runtime.assessment().relation == PermittedAreaRelation::kInside);
  assert(runtime.assessedObservationCapturedAtMs() == 55);

  GeoPointE7 too_many_unique[65]{};
  for (uint16_t i = 0; i < 65; ++i) {
    too_many_unique[i] = GeoPointE7(i, static_cast<int32_t>(i * i + 1));
  }
  const GeofencePolygonView polygon_too_large[] = {polygon(too_many_unique, 65)};
  assert(runtime.configure(GeofenceAreaSetView(polygon_too_large, 1)) ==
         GeofenceRuntimeConfigResult::kTooManyVertices);
  assert(runtime.areaCount() == 1 && runtime.totalVertexCount() == 4);
  assert(runtime.hasAssessment());
  assert(runtime.assessment().relation == PermittedAreaRelation::kInside);
  assert(runtime.assessedObservationCapturedAtMs() == 55);
}

void successfulReplacementAndExplicitClearResetAssessment() {
  const GeoPointE7 first[] = {
      GeoPointE7(0, 0), GeoPointE7(0, 1000),
      GeoPointE7(1000, 1000), GeoPointE7(1000, 0)};
  const GeofencePolygonView first_area[] = {polygon(first, 4)};
  GeofenceRuntime runtime;
  assert(runtime.configure(GeofenceAreaSetView(first_area, 1)) ==
         GeofenceRuntimeConfigResult::kApplied);
  assert(observeAt(runtime, 500, 500, UINT32_MAX - 3) ==
         GeofenceObservationResult::kAccepted);
  assert(runtime.hasAssessment());

  const GeoPointE7 second[] = {
      GeoPointE7(5000, 5000), GeoPointE7(5000, 6000),
      GeoPointE7(6000, 6000), GeoPointE7(6000, 5000)};
  const GeofencePolygonView second_area[] = {polygon(second, 4)};
  assert(runtime.configure(GeofenceAreaSetView(second_area, 1)) ==
         GeofenceRuntimeConfigResult::kApplied);
  assert(!runtime.hasAssessment());
  assert(observeAt(runtime, 5500, 5500, 2) ==
         GeofenceObservationResult::kAccepted);
  assert(runtime.assessedObservationCapturedAtMs() == 2);

  runtime.clear();
  assert(!runtime.configured());
  assert(runtime.areaCount() == 0);
  assert(runtime.totalVertexCount() == 0);
  assert(!runtime.hasAssessment());
  assert(observeAt(runtime, 5500, 5500, 3) ==
         GeofenceObservationResult::kNotConfigured);
}

}  // namespace

int main() {
  unconfiguredRuntimeDoesNotInventState();
  boundedOwnedCopyAndUnionAssessment();
  explicitClosingVertexUsesEffectiveVertexBudget();
  invalidObservationDoesNotEraseLastValidAssessment();
  rejectedReplacementPreservesKnownGoodConfigAndResult();
  resourceBoundsRejectWithoutMutatingActiveConfig();
  successfulReplacementAndExplicitClearResetAssessment();
  puts("M6C3 bounded geofence runtime checks: PASS");
}
