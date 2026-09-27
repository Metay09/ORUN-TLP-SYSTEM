#include <assert.h>
#include <stdio.h>

#include "geofence_operational_state.h"

using namespace orun_tlp;

namespace {

void expectState(const GeofenceOperationalStateMachine& machine,
                 GeofenceOperationalState expected) {
  GeofenceOperationalState actual = GeofenceOperationalState::kInside;
  assert(machine.getConfirmedState(&actual));
  assert(actual == expected);
}

void establishOutside(GeofenceOperationalStateMachine& machine) {
  assert(machine.observe(PermittedAreaRelation::kOutside) ==
         GeofenceOperationalResult::kConfirmationStarted);
  assert(machine.observe(PermittedAreaRelation::kOutside) ==
         GeofenceOperationalResult::kConfirmationContinues);
  assert(machine.observe(PermittedAreaRelation::kOutside) ==
         GeofenceOperationalResult::kInitializedOutside);
  expectState(machine, GeofenceOperationalState::kOutside);
  assert(machine.cadenceMode() == GeofenceCadenceMode::kBaseDividedBy3);
}

void initialClassificationIsFailSafeForOutside() {
  GeofenceOperationalStateMachine machine;
  assert(!machine.hasConfirmedState());
  assert(machine.cadenceMode() == GeofenceCadenceMode::kBase);

  assert(machine.observe(PermittedAreaRelation::kBoundary) ==
         GeofenceOperationalResult::kBoundaryNoDecision);
  assert(!machine.hasConfirmedState());

  assert(machine.observe(PermittedAreaRelation::kInside) ==
         GeofenceOperationalResult::kInitializedInside);
  expectState(machine, GeofenceOperationalState::kInside);
  assert(machine.cadenceMode() == GeofenceCadenceMode::kBase);

  machine.reset();
  assert(machine.observe(PermittedAreaRelation::kOutside) ==
         GeofenceOperationalResult::kConfirmationStarted);
  assert(machine.confirmationActive());
  assert(machine.confirmationObservationCount() == 1);
  assert(machine.confirmationOutsideVotes() == 1);

  assert(machine.observe(PermittedAreaRelation::kOutside) ==
         GeofenceOperationalResult::kConfirmationContinues);
  assert(!machine.hasConfirmedState());  // Full 3 observations are intentional.
  assert(machine.observe(PermittedAreaRelation::kOutside) ==
         GeofenceOperationalResult::kInitializedOutside);
  expectState(machine, GeofenceOperationalState::kOutside);
  assert(machine.cadenceMode() == GeofenceCadenceMode::kBaseDividedBy3);
}

void insideToOutsideRequiresFullThreeObservationMajority() {
  GeofenceOperationalStateMachine machine;
  assert(machine.observe(PermittedAreaRelation::kInside) ==
         GeofenceOperationalResult::kInitializedInside);

  assert(machine.observe(PermittedAreaRelation::kOutside) ==
         GeofenceOperationalResult::kConfirmationStarted);
  assert(machine.observe(PermittedAreaRelation::kOutside) ==
         GeofenceOperationalResult::kConfirmationContinues);
  expectState(machine, GeofenceOperationalState::kInside);

  assert(machine.observe(PermittedAreaRelation::kInside) ==
         GeofenceOperationalResult::kConfirmedOutside);
  // Votes were OUTSIDE, OUTSIDE, INSIDE: 2-of-3 OUTSIDE.
  expectState(machine, GeofenceOperationalState::kOutside);

  assert(machine.observe(PermittedAreaRelation::kOutside) ==
         GeofenceOperationalResult::kStableOutside);
  assert(!machine.confirmationActive());  // No duplicate transition occurrence.
}

void initializedOutsideIsDistinctFromInsideToOutsideTransition() {
  GeofenceOperationalStateMachine machine;
  establishOutside(machine);

  machine.reset();
  assert(machine.observe(PermittedAreaRelation::kInside) ==
         GeofenceOperationalResult::kInitializedInside);
  assert(machine.observe(PermittedAreaRelation::kOutside) ==
         GeofenceOperationalResult::kConfirmationStarted);
  assert(machine.observe(PermittedAreaRelation::kOutside) ==
         GeofenceOperationalResult::kConfirmationContinues);
  assert(machine.observe(PermittedAreaRelation::kInside) ==
         GeofenceOperationalResult::kConfirmedOutside);
}

void falseOutsideCandidateIsRejected() {
  GeofenceOperationalStateMachine machine;
  assert(machine.observe(PermittedAreaRelation::kInside) ==
         GeofenceOperationalResult::kInitializedInside);

  assert(machine.observe(PermittedAreaRelation::kOutside) ==
         GeofenceOperationalResult::kConfirmationStarted);
  assert(machine.observe(PermittedAreaRelation::kInside) ==
         GeofenceOperationalResult::kConfirmationContinues);
  assert(machine.observe(PermittedAreaRelation::kInside) ==
         GeofenceOperationalResult::kConfirmationRejected);
  expectState(machine, GeofenceOperationalState::kInside);
  assert(machine.cadenceMode() == GeofenceCadenceMode::kBase);
}

void boundaryIsNeutralButBoundedInsideConfirmation() {
  GeofenceOperationalStateMachine machine;
  assert(machine.observe(PermittedAreaRelation::kInside) ==
         GeofenceOperationalResult::kInitializedInside);

  assert(machine.observe(PermittedAreaRelation::kOutside) ==
         GeofenceOperationalResult::kConfirmationStarted);
  assert(machine.observe(PermittedAreaRelation::kBoundary) ==
         GeofenceOperationalResult::kConfirmationContinues);
  assert(machine.confirmationObservationCount() == 2);
  assert(machine.confirmationOutsideVotes() == 1);
  assert(machine.confirmationInsideVotes() == 0);

  assert(machine.observe(PermittedAreaRelation::kOutside) ==
         GeofenceOperationalResult::kConfirmedOutside);
  expectState(machine, GeofenceOperationalState::kOutside);

  machine.reset();
  assert(machine.observe(PermittedAreaRelation::kInside) ==
         GeofenceOperationalResult::kInitializedInside);
  assert(machine.observe(PermittedAreaRelation::kOutside) ==
         GeofenceOperationalResult::kConfirmationStarted);
  assert(machine.observe(PermittedAreaRelation::kBoundary) ==
         GeofenceOperationalResult::kConfirmationContinues);
  assert(machine.observe(PermittedAreaRelation::kBoundary) ==
         GeofenceOperationalResult::kConfirmationRejected);
  expectState(machine, GeofenceOperationalState::kInside);
}

void boundaryIsNeutralInOutsideAndUnclassifiedEpisodes() {
  GeofenceOperationalStateMachine machine;
  establishOutside(machine);

  assert(machine.observe(PermittedAreaRelation::kInside) ==
         GeofenceOperationalResult::kConfirmationStarted);
  assert(machine.observe(PermittedAreaRelation::kBoundary) ==
         GeofenceOperationalResult::kConfirmationContinues);
  assert(machine.confirmationInsideVotes() == 1);
  assert(machine.confirmationOutsideVotes() == 0);
  assert(machine.observe(PermittedAreaRelation::kInside) ==
         GeofenceOperationalResult::kConfirmedInside);
  expectState(machine, GeofenceOperationalState::kInside);

  machine.reset();
  assert(machine.observe(PermittedAreaRelation::kOutside) ==
         GeofenceOperationalResult::kConfirmationStarted);
  assert(machine.observe(PermittedAreaRelation::kBoundary) ==
         GeofenceOperationalResult::kConfirmationContinues);
  assert(machine.confirmationObservationCount() == 2);
  assert(machine.observe(PermittedAreaRelation::kOutside) ==
         GeofenceOperationalResult::kInitializedOutside);
  expectState(machine, GeofenceOperationalState::kOutside);
}

void outsideToInsideUsesSameBoundedMajority() {
  GeofenceOperationalStateMachine machine;
  establishOutside(machine);

  assert(machine.observe(PermittedAreaRelation::kInside) ==
         GeofenceOperationalResult::kConfirmationStarted);
  assert(machine.observe(PermittedAreaRelation::kOutside) ==
         GeofenceOperationalResult::kConfirmationContinues);
  assert(machine.observe(PermittedAreaRelation::kInside) ==
         GeofenceOperationalResult::kConfirmedInside);
  expectState(machine, GeofenceOperationalState::kInside);
  assert(machine.cadenceMode() == GeofenceCadenceMode::kBase);
}

void invalidObservationsDoNotConsumeEpisodeBudget() {
  GeofenceOperationalStateMachine machine;
  assert(machine.observe(PermittedAreaRelation::kInside) ==
         GeofenceOperationalResult::kInitializedInside);
  assert(machine.observe(PermittedAreaRelation::kOutside) ==
         GeofenceOperationalResult::kConfirmationStarted);
  assert(machine.confirmationObservationCount() == 1);

  assert(machine.observe(PermittedAreaRelation::kInvalidPoint) ==
         GeofenceOperationalResult::kInvalidObservation);
  assert(machine.observe(PermittedAreaRelation::kInvalidAreaSet) ==
         GeofenceOperationalResult::kInvalidObservation);
  assert(machine.confirmationObservationCount() == 1);

  assert(machine.observe(PermittedAreaRelation::kInside) ==
         GeofenceOperationalResult::kConfirmationContinues);
  assert(machine.observe(PermittedAreaRelation::kInside) ==
         GeofenceOperationalResult::kConfirmationRejected);
  expectState(machine, GeofenceOperationalState::kInside);
}

void timeoutAbortPreservesPreviousAuthorityAndCadence() {
  GeofenceOperationalStateMachine machine;
  assert(!machine.abortConfirmation());

  assert(machine.observe(PermittedAreaRelation::kInside) ==
         GeofenceOperationalResult::kInitializedInside);
  assert(machine.observe(PermittedAreaRelation::kOutside) ==
         GeofenceOperationalResult::kConfirmationStarted);
  assert(machine.abortConfirmation());
  assert(!machine.confirmationActive());
  expectState(machine, GeofenceOperationalState::kInside);
  assert(machine.cadenceMode() == GeofenceCadenceMode::kBase);

  machine.reset();
  assert(machine.observe(PermittedAreaRelation::kOutside) ==
         GeofenceOperationalResult::kConfirmationStarted);
  assert(machine.abortConfirmation());
  assert(!machine.hasConfirmedState());
  assert(machine.cadenceMode() == GeofenceCadenceMode::kBase);

  establishOutside(machine);
  assert(machine.observe(PermittedAreaRelation::kInside) ==
         GeofenceOperationalResult::kConfirmationStarted);
  assert(machine.abortConfirmation());
  assert(!machine.confirmationActive());
  expectState(machine, GeofenceOperationalState::kOutside);
  assert(machine.cadenceMode() == GeofenceCadenceMode::kBaseDividedBy3);
}

void resetInvalidatesOldGeometryEvidenceAndActiveEpisode() {
  GeofenceOperationalStateMachine machine;
  establishOutside(machine);

  machine.reset();
  assert(!machine.hasConfirmedState());
  assert(!machine.confirmationActive());
  assert(machine.cadenceMode() == GeofenceCadenceMode::kBase);

  assert(machine.observe(PermittedAreaRelation::kInside) ==
         GeofenceOperationalResult::kInitializedInside);
  assert(machine.observe(PermittedAreaRelation::kOutside) ==
         GeofenceOperationalResult::kConfirmationStarted);
  assert(machine.confirmationActive());

  machine.reset();
  assert(!machine.hasConfirmedState());
  assert(!machine.confirmationActive());
  assert(machine.confirmationObservationCount() == 0);
  assert(machine.cadenceMode() == GeofenceCadenceMode::kBase);

  assert(machine.observe(PermittedAreaRelation::kInside) ==
         GeofenceOperationalResult::kInitializedInside);
  expectState(machine, GeofenceOperationalState::kInside);
}

void unclassifiedOutsideEpisodeCanResolveInside() {
  GeofenceOperationalStateMachine machine;
  assert(machine.observe(PermittedAreaRelation::kOutside) ==
         GeofenceOperationalResult::kConfirmationStarted);
  assert(machine.observe(PermittedAreaRelation::kInside) ==
         GeofenceOperationalResult::kConfirmationContinues);
  assert(machine.observe(PermittedAreaRelation::kInside) ==
         GeofenceOperationalResult::kInitializedInside);
  expectState(machine, GeofenceOperationalState::kInside);
}

}  // namespace

int main() {
  initialClassificationIsFailSafeForOutside();
  insideToOutsideRequiresFullThreeObservationMajority();
  initializedOutsideIsDistinctFromInsideToOutsideTransition();
  falseOutsideCandidateIsRejected();
  boundaryIsNeutralButBoundedInsideConfirmation();
  boundaryIsNeutralInOutsideAndUnclassifiedEpisodes();
  outsideToInsideUsesSameBoundedMajority();
  invalidObservationsDoNotConsumeEpisodeBudget();
  timeoutAbortPreservesPreviousAuthorityAndCadence();
  resetInvalidatesOldGeometryEvidenceAndActiveEpisode();
  unclassifiedOutsideEpisodeCanResolveInside();
  puts("M6D1 geofence operational-state checks: PASS");
}
