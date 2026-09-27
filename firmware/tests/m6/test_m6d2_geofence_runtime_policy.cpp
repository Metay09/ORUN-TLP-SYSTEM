#include <assert.h>
#include <stdio.h>

#include "geofence_runtime_policy.h"

using namespace orun_tlp;
using namespace orun_tlp::geofence_runtime_policy;

namespace {

void cadenceUsesCeilingDivisionAndNeverTouchesBase() {
  assert(effectiveTrackingIntervalMs(180, GeofenceCadenceMode::kBase) == 180000);
  assert(effectiveTrackingIntervalMs(180, GeofenceCadenceMode::kBaseDividedBy3) ==
         60000);
  assert(effectiveTrackingIntervalMs(181, GeofenceCadenceMode::kBaseDividedBy3) ==
         61000);
  assert(effectiveTrackingIntervalMs(182, GeofenceCadenceMode::kBaseDividedBy3) ==
         61000);
  assert(effectiveTrackingIntervalMs(1, GeofenceCadenceMode::kBaseDividedBy3) ==
         1000);
  assert(effectiveTrackingIntervalMs(0, GeofenceCadenceMode::kBase) == 0);
  assert(effectiveTrackingIntervalMs(UINT32_MAX, GeofenceCadenceMode::kBase) == 0);
}

void representativeUsesOnlyWinningRealObservation() {
  const RepresentativeCandidate candidates[] = {
      {PermittedAreaRelation::kOutside, 95, 10, 0},
      {PermittedAreaRelation::kOutside, 80, 6, 1},
      {PermittedAreaRelation::kInside, 40, 12, 2},
  };
  uint8_t index = 99;
  assert(selectRepresentative(candidates, 3, PermittedAreaRelation::kOutside,
                              &index));
  assert(index == 1);
  assert(selectRepresentative(candidates, 3, PermittedAreaRelation::kInside,
                              &index));
  assert(index == 2);
}

void representativeTieBreakIsDeterministic() {
  const RepresentativeCandidate candidates[] = {
      {PermittedAreaRelation::kOutside, 80, 7, 0},
      {PermittedAreaRelation::kOutside, 80, 9, 1},
      {PermittedAreaRelation::kOutside, 80, 9, 2},
  };
  uint8_t index = 99;
  assert(selectRepresentative(candidates, 3, PermittedAreaRelation::kOutside,
                              &index));
  assert(index == 2);
}

void invalidInputsFailClosed() {
  const RepresentativeCandidate candidates[] = {
      {PermittedAreaRelation::kBoundary, 10, 20, 0},
      {PermittedAreaRelation::kInvalidPoint, 1, 30, 1},
  };
  uint8_t index = 99;
  assert(!selectRepresentative(nullptr, 2, PermittedAreaRelation::kOutside,
                               &index));
  assert(!selectRepresentative(candidates, 0, PermittedAreaRelation::kOutside,
                               &index));
  assert(!selectRepresentative(candidates, 2, PermittedAreaRelation::kBoundary,
                               &index));
  assert(!selectRepresentative(candidates, 2, PermittedAreaRelation::kOutside,
                               &index));
  assert(!selectRepresentative(candidates, 2, PermittedAreaRelation::kInside,
                               nullptr));
}

}  // namespace

int main() {
  cadenceUsesCeilingDivisionAndNeverTouchesBase();
  representativeUsesOnlyWinningRealObservation();
  representativeTieBreakIsDeterministic();
  invalidInputsFailClosed();
  puts("M6D2 geofence runtime-policy checks: PASS");
}
