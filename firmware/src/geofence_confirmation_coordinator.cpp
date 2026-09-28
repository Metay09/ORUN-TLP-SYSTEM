#include "geofence_confirmation_coordinator.h"

#include "monotonic_time.h"

namespace orun_tlp {

GeofenceRuntimeConfigResult GeofenceConfirmationCoordinator::configure(
    const GeofenceAreaSetView& candidate) {
  const GeofenceRuntimeConfigResult result = runtime_.configure(candidate);
  if (result == GeofenceRuntimeConfigResult::kApplied) {
    operational_.reset();
    clearEpisodeEvidence();
  }
  return result;
}

void GeofenceConfirmationCoordinator::clear() {
  runtime_.clear();
  operational_.reset();
  clearEpisodeEvidence();
}

void GeofenceConfirmationCoordinator::clearEpisodeEvidence() {
  evidence_count_ = 0;
  confirmation_started_at_ms_ = 0;
  confirmation_deadline_active_ = false;
}

void GeofenceConfirmationCoordinator::addEpisodeEvidence(
    PermittedAreaRelation relation, uint16_t hdop_x100, uint8_t satellites) {
  if (evidence_count_ >=
      geofence_operational_config::kConfirmationObservationLimit) {
    return;
  }
  evidence_[evidence_count_] =
      geofence_runtime_policy::RepresentativeCandidate(
          relation, hdop_x100, satellites, evidence_count_);
  ++evidence_count_;
}

bool GeofenceConfirmationCoordinator::chooseRepresentative(
    GeofenceOperationalResult result, uint8_t* representative_slot) const {
  PermittedAreaRelation winning_relation = PermittedAreaRelation::kInvalidPoint;
  switch (result) {
    case GeofenceOperationalResult::kConfirmedOutside:
    case GeofenceOperationalResult::kInitializedOutside:
      winning_relation = PermittedAreaRelation::kOutside;
      break;
    case GeofenceOperationalResult::kConfirmedInside:
    case GeofenceOperationalResult::kInitializedInside:
      winning_relation = PermittedAreaRelation::kInside;
      break;
    default:
      return false;
  }

  return geofence_runtime_policy::selectRepresentative(
      evidence_, evidence_count_, winning_relation, representative_slot);
}

GeofenceConfirmationUpdate
GeofenceConfirmationCoordinator::observeAcceptedLocation(
    const GeoPointE7& point, uint32_t captured_at_ms, uint16_t hdop_x100,
    uint8_t satellites) {
  GeofenceConfirmationUpdate update;
  update.cadence_mode = operational_.cadenceMode();

  const bool was_active = operational_.confirmationActive();
  const GeofenceObservationResult geometry_result =
      runtime_.observeAcceptedPosition(point, captured_at_ms);
  update.geometry_result = geometry_result;

  // The loop-level deadline check prevents ordinary late service, but a GNSS
  // poll can itself cross the boundary before delivering a callback. Enforce
  // the same deadline against the observation capture timestamp here, before
  // the observation can consume evidence or decide a transition.
  if (was_active && confirmation_deadline_active_ &&
      monotonic::elapsed(captured_at_ms, confirmation_started_at_ms_,
                         geofence_confirmation_config::kConfirmationDeadlineMs)) {
    update.confirmation_timed_out = abortConfirmation();
    update.cadence_mode = operational_.cadenceMode();
    return update;
  }

  if (geometry_result != GeofenceObservationResult::kAccepted) {
    // An unsupported/invalid point is not transition evidence and therefore
    // does not consume one of the three slots. While an episode is already
    // active, request another accepted observation within the same deadline.
    // A config fault is different: continuing against suspect geometry would
    // be unsafe, so the composition root will abort the episode.
    update.request_additional_observation =
        was_active && geometry_result == GeofenceObservationResult::kInvalidPoint;
    return update;
  }

  const PermittedAreaRelation relation = runtime_.assessment().relation;
  const GeofenceCadenceMode cadence_before = operational_.cadenceMode();
  const uint8_t slot_before = evidence_count_;

  const GeofenceOperationalResult operational_result =
      operational_.observe(relation);
  update.operational_result = operational_result;

  if (!was_active &&
      operational_result == GeofenceOperationalResult::kConfirmationStarted) {
    clearEpisodeEvidence();
    confirmation_started_at_ms_ = captured_at_ms;
    confirmation_deadline_active_ = true;
    addEpisodeEvidence(relation, hdop_x100, satellites);
    update.episode_evidence_accepted = true;
    update.episode_slot = 0;
  } else if (was_active) {
    addEpisodeEvidence(relation, hdop_x100, satellites);
    update.episode_evidence_accepted =
        evidence_count_ == static_cast<uint8_t>(slot_before + 1U);
    update.episode_slot = slot_before;
  }

  update.request_additional_observation = operational_.confirmationActive();
  update.outside_event_occurrence =
      operational_result == GeofenceOperationalResult::kConfirmedOutside;

  const GeofenceCadenceMode cadence_after = operational_.cadenceMode();
  update.cadence_mode = cadence_after;
  update.cadence_changed = cadence_after != cadence_before;

  if (!operational_.confirmationActive() && was_active) {
    uint8_t representative_slot = 0;
    if (chooseRepresentative(operational_result, &representative_slot)) {
      update.representative_available = true;
      update.representative_slot = representative_slot;
    }
    clearEpisodeEvidence();
  }

  return update;
}

bool GeofenceConfirmationCoordinator::expireConfirmation(uint32_t now_ms) {
  if (!confirmation_deadline_active_ || !operational_.confirmationActive()) {
    return false;
  }
  if (!monotonic::elapsed(now_ms, confirmation_started_at_ms_,
                          geofence_confirmation_config::kConfirmationDeadlineMs)) {
    return false;
  }

  return abortConfirmation();
}

bool GeofenceConfirmationCoordinator::abortConfirmation() {
  if (!operational_.abortConfirmation()) return false;
  clearEpisodeEvidence();
  return true;
}

}  // namespace orun_tlp
