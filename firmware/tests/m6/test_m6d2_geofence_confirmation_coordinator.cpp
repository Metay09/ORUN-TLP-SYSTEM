#include <assert.h>
#include <stdio.h>

#include "geofence_confirmation_coordinator.h"

using namespace orun_tlp;

namespace {

const GeoPointE7 kSquare[] = {
    GeoPointE7(0, 0), GeoPointE7(0, 1000),
    GeoPointE7(1000, 1000), GeoPointE7(1000, 0)};
const GeofencePolygonView kAreas[] = {GeofencePolygonView(kSquare, 4)};

GeoPointE7 inside() { return GeoPointE7(500, 500); }
GeoPointE7 outsideA() { return GeoPointE7(1500, 1500); }
GeoPointE7 outsideB() { return GeoPointE7(1600, 1600); }

void stableInsideNeedsNoBurst() {
  GeofenceConfirmationCoordinator c;
  assert(c.configure(GeofenceAreaSetView(kAreas, 1)) ==
         GeofenceRuntimeConfigResult::kApplied);

  const auto first = c.observeAcceptedLocation(inside(), 100, 90, 8);
  assert(first.geometry_result == GeofenceObservationResult::kAccepted);
  assert(first.operational_result ==
         GeofenceOperationalResult::kInitializedInside);
  assert(!first.request_additional_observation);
  assert(!first.representative_available);
  assert(!first.outside_event_occurrence);
  assert(c.cadenceMode() == GeofenceCadenceMode::kBase);

  const auto stable = c.observeAcceptedLocation(inside(), 1000, 80, 9);
  assert(stable.operational_result == GeofenceOperationalResult::kStableInside);
  assert(!stable.request_additional_observation);
}

void confirmedOutsideUsesThreeSlotsAndOneRealRepresentative() {
  GeofenceConfirmationCoordinator c;
  assert(c.configure(GeofenceAreaSetView(kAreas, 1)) ==
         GeofenceRuntimeConfigResult::kApplied);
  assert(c.observeAcceptedLocation(inside(), 100, 100, 7).operational_result ==
         GeofenceOperationalResult::kInitializedInside);

  const auto one = c.observeAcceptedLocation(outsideA(), 1000, 120, 10);
  assert(one.operational_result ==
         GeofenceOperationalResult::kConfirmationStarted);
  assert(one.request_additional_observation);
  assert(one.episode_evidence_accepted && one.episode_slot == 0);

  const auto two = c.observeAcceptedLocation(outsideB(), 2000, 70, 6);
  assert(two.operational_result ==
         GeofenceOperationalResult::kConfirmationContinues);
  assert(two.request_additional_observation);
  assert(two.episode_evidence_accepted && two.episode_slot == 1);

  const auto three = c.observeAcceptedLocation(inside(), 3000, 40, 12);
  assert(three.operational_result ==
         GeofenceOperationalResult::kConfirmedOutside);
  assert(!three.request_additional_observation);
  assert(three.episode_evidence_accepted && three.episode_slot == 2);
  assert(three.representative_available);
  // Winning OUTSIDE observations are slots 0/1. Slot 1 wins lower HDOP even
  // though slot 2 (INSIDE) has globally better quality.
  assert(three.representative_slot == 1);
  assert(three.outside_event_occurrence);
  assert(three.cadence_changed);
  assert(three.cadence_mode == GeofenceCadenceMode::kBaseDividedBy3);
}

void falseOutsideCandidatePreservesInsideWithoutExtraRepresentative() {
  GeofenceConfirmationCoordinator c;
  assert(c.configure(GeofenceAreaSetView(kAreas, 1)) ==
         GeofenceRuntimeConfigResult::kApplied);
  c.observeAcceptedLocation(inside(), 100, 90, 8);

  assert(c.observeAcceptedLocation(outsideA(), 1000, 90, 8)
             .request_additional_observation);
  assert(c.observeAcceptedLocation(inside(), 2000, 80, 9)
             .request_additional_observation);
  const auto final = c.observeAcceptedLocation(inside(), 3000, 70, 10);
  assert(final.operational_result ==
         GeofenceOperationalResult::kConfirmationRejected);
  assert(!final.representative_available);
  assert(!final.outside_event_occurrence);
  assert(!final.cadence_changed);
  assert(c.cadenceMode() == GeofenceCadenceMode::kBase);
}

void invalidPointDoesNotConsumeEpisodeAndRequestsReplacement() {
  GeofenceConfirmationCoordinator c;
  assert(c.configure(GeofenceAreaSetView(kAreas, 1)) ==
         GeofenceRuntimeConfigResult::kApplied);
  c.observeAcceptedLocation(inside(), 100, 100, 7);
  c.observeAcceptedLocation(outsideA(), 1000, 100, 7);
  assert(c.confirmationActive());

  const auto invalid =
      c.observeAcceptedLocation(GeoPointE7(900000000, 0), 2000, 100, 7);
  assert(invalid.geometry_result == GeofenceObservationResult::kInvalidPoint);
  assert(invalid.request_additional_observation);
  assert(!invalid.episode_evidence_accepted);
  assert(c.confirmationActive());

  const auto second = c.observeAcceptedLocation(outsideB(), 3000, 80, 8);
  assert(second.operational_result ==
         GeofenceOperationalResult::kConfirmationContinues);
  assert(second.episode_slot == 1);
}

void initializedOutsideIsNotPhysicalOutsideEvent() {
  GeofenceConfirmationCoordinator c;
  assert(c.configure(GeofenceAreaSetView(kAreas, 1)) ==
         GeofenceRuntimeConfigResult::kApplied);

  c.observeAcceptedLocation(outsideA(), 1000, 100, 7);
  c.observeAcceptedLocation(outsideB(), 2000, 80, 8);
  const auto final = c.observeAcceptedLocation(outsideA(), 3000, 90, 9);
  assert(final.operational_result ==
         GeofenceOperationalResult::kInitializedOutside);
  assert(final.representative_available);
  assert(final.representative_slot == 1);
  assert(!final.outside_event_occurrence);
  assert(final.cadence_changed);
  assert(c.cadenceMode() == GeofenceCadenceMode::kBaseDividedBy3);
}

void exactDeadlineAbortsAndPreservesPriorAuthority() {
  GeofenceConfirmationCoordinator c;
  assert(c.configure(GeofenceAreaSetView(kAreas, 1)) ==
         GeofenceRuntimeConfigResult::kApplied);
  c.observeAcceptedLocation(inside(), 10, 100, 7);
  c.observeAcceptedLocation(outsideA(), 1000, 100, 7);
  assert(c.confirmationActive());

  assert(!c.expireConfirmation(
      1000 + geofence_confirmation_config::kConfirmationDeadlineMs - 1));
  assert(c.confirmationActive());
  assert(c.expireConfirmation(
      1000 + geofence_confirmation_config::kConfirmationDeadlineMs));
  assert(!c.confirmationActive());
  assert(c.cadenceMode() == GeofenceCadenceMode::kBase);
  assert(!c.expireConfirmation(
      1000 + geofence_confirmation_config::kConfirmationDeadlineMs + 1));
}

void lateCapturedObservationCannotWinDeadlineRace() {
  GeofenceConfirmationCoordinator c;
  assert(c.configure(GeofenceAreaSetView(kAreas, 1)) ==
         GeofenceRuntimeConfigResult::kApplied);
  c.observeAcceptedLocation(inside(), 100, 100, 7);
  c.observeAcceptedLocation(outsideA(), 1000, 100, 7);
  c.observeAcceptedLocation(outsideB(), 2000, 90, 8);
  assert(c.confirmationActive());

  const auto late = c.observeAcceptedLocation(
      outsideA(),
      1000 + geofence_confirmation_config::kConfirmationDeadlineMs,
      80, 9);
  assert(late.geometry_result == GeofenceObservationResult::kAccepted);
  assert(late.confirmation_timed_out);
  assert(!late.representative_available);
  assert(!late.outside_event_occurrence);
  assert(!late.cadence_changed);
  assert(!late.request_additional_observation);
  assert(!c.confirmationActive());
  assert(c.cadenceMode() == GeofenceCadenceMode::kBase);
}

void acquisitionOwnerAbortPreservesAuthority() {
  GeofenceConfirmationCoordinator c;
  assert(c.configure(GeofenceAreaSetView(kAreas, 1)) ==
         GeofenceRuntimeConfigResult::kApplied);
  c.observeAcceptedLocation(inside(), 100, 100, 7);
  c.observeAcceptedLocation(outsideA(), 1000, 100, 7);
  assert(c.confirmationActive());
  assert(c.abortConfirmation());
  assert(!c.confirmationActive());
  assert(c.cadenceMode() == GeofenceCadenceMode::kBase);
  assert(!c.abortConfirmation());
}

void configReplacementAndClearInvalidateOldEvidence() {
  GeofenceConfirmationCoordinator c;
  assert(c.configure(GeofenceAreaSetView(kAreas, 1)) ==
         GeofenceRuntimeConfigResult::kApplied);
  c.observeAcceptedLocation(inside(), 100, 100, 7);
  c.observeAcceptedLocation(outsideA(), 1000, 100, 7);
  assert(c.confirmationActive());

  // Successful replacement resets operational evidence; rejected replacement
  // would preserve the last known-good runtime/state because configure() only
  // resets after kApplied.
  assert(c.configure(GeofenceAreaSetView(kAreas, 1)) ==
         GeofenceRuntimeConfigResult::kApplied);
  assert(!c.confirmationActive());
  assert(c.cadenceMode() == GeofenceCadenceMode::kBase);

  c.clear();
  assert(!c.configured());
  const auto ignored = c.observeAcceptedLocation(inside(), 2000, 100, 7);
  assert(ignored.geometry_result == GeofenceObservationResult::kNotConfigured);
}

}  // namespace

int main() {
  stableInsideNeedsNoBurst();
  confirmedOutsideUsesThreeSlotsAndOneRealRepresentative();
  falseOutsideCandidatePreservesInsideWithoutExtraRepresentative();
  invalidPointDoesNotConsumeEpisodeAndRequestsReplacement();
  initializedOutsideIsNotPhysicalOutsideEvent();
  exactDeadlineAbortsAndPreservesPriorAuthority();
  lateCapturedObservationCannotWinDeadlineRace();
  acquisitionOwnerAbortPreservesAuthority();
  configReplacementAndClearInvalidateOldEvidence();
  puts("M6D2 geofence confirmation coordinator checks: PASS");
}
