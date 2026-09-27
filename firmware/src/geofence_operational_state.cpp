#include "geofence_operational_state.h"

namespace orun_tlp {

namespace {

bool isAcceptedOperationalRelation(PermittedAreaRelation relation) {
  return relation == PermittedAreaRelation::kInside ||
         relation == PermittedAreaRelation::kBoundary ||
         relation == PermittedAreaRelation::kOutside;
}

}  // namespace

bool GeofenceOperationalStateMachine::getConfirmedState(
    GeofenceOperationalState* state) const {
  if (state == nullptr || !has_confirmed_state_) return false;
  *state = confirmed_state_;
  return true;
}

void GeofenceOperationalStateMachine::clearConfirmation() {
  confirmation_active_ = false;
  confirmation_origin_ = ConfirmationOrigin::kNone;
  confirmation_observation_count_ = 0;
  confirmation_inside_votes_ = 0;
  confirmation_outside_votes_ = 0;
}

void GeofenceOperationalStateMachine::startConfirmation(
    ConfirmationOrigin origin, PermittedAreaRelation first_relation) {
  clearConfirmation();
  confirmation_active_ = true;
  confirmation_origin_ = origin;
  addConfirmationObservation(first_relation);
}

void GeofenceOperationalStateMachine::addConfirmationObservation(
    PermittedAreaRelation relation) {
  ++confirmation_observation_count_;
  if (relation == PermittedAreaRelation::kInside) {
    ++confirmation_inside_votes_;
  } else if (relation == PermittedAreaRelation::kOutside) {
    ++confirmation_outside_votes_;
  }
  // BOUNDARY intentionally consumes one bounded evidence slot but contributes
  // no INSIDE/OUTSIDE vote. This prevents an exact/near-edge sequence from
  // creating an unbounded confirmation burst or a false OUTSIDE transition.
}

GeofenceOperationalResult
GeofenceOperationalStateMachine::finishConfirmation() {
  const ConfirmationOrigin origin = confirmation_origin_;
  const uint8_t inside_votes = confirmation_inside_votes_;
  const uint8_t outside_votes = confirmation_outside_votes_;
  clearConfirmation();

  if (origin == ConfirmationOrigin::kInside) {
    if (outside_votes >= geofence_operational_config::kTransitionVotesRequired) {
      has_confirmed_state_ = true;
      confirmed_state_ = GeofenceOperationalState::kOutside;
      return GeofenceOperationalResult::kConfirmedOutside;
    }
    return GeofenceOperationalResult::kConfirmationRejected;
  }

  if (origin == ConfirmationOrigin::kOutside) {
    if (inside_votes >= geofence_operational_config::kTransitionVotesRequired) {
      has_confirmed_state_ = true;
      confirmed_state_ = GeofenceOperationalState::kInside;
      return GeofenceOperationalResult::kConfirmedInside;
    }
    return GeofenceOperationalResult::kConfirmationRejected;
  }

  if (origin == ConfirmationOrigin::kUnclassified) {
    if (outside_votes >= geofence_operational_config::kTransitionVotesRequired) {
      has_confirmed_state_ = true;
      confirmed_state_ = GeofenceOperationalState::kOutside;
      return GeofenceOperationalResult::kConfirmedOutside;
    }
    if (inside_votes >= geofence_operational_config::kTransitionVotesRequired) {
      has_confirmed_state_ = true;
      confirmed_state_ = GeofenceOperationalState::kInside;
      return GeofenceOperationalResult::kInitializedInside;
    }
    return GeofenceOperationalResult::kConfirmationRejected;
  }

  return GeofenceOperationalResult::kConfirmationRejected;
}

GeofenceOperationalResult GeofenceOperationalStateMachine::observe(
    PermittedAreaRelation relation) {
  if (!isAcceptedOperationalRelation(relation)) {
    return GeofenceOperationalResult::kInvalidObservation;
  }

  if (confirmation_active_) {
    addConfirmationObservation(relation);
    if (confirmation_observation_count_ <
        geofence_operational_config::kConfirmationObservationLimit) {
      return GeofenceOperationalResult::kConfirmationContinues;
    }
    return finishConfirmation();
  }

  if (!has_confirmed_state_) {
    if (relation == PermittedAreaRelation::kBoundary) {
      return GeofenceOperationalResult::kBoundaryNoDecision;
    }
    if (relation == PermittedAreaRelation::kInside) {
      has_confirmed_state_ = true;
      confirmed_state_ = GeofenceOperationalState::kInside;
      return GeofenceOperationalResult::kInitializedInside;
    }

    // First OUTSIDE evidence after boot/config replacement is never enough to
    // create confirmed OUTSIDE. It starts the same full 3-observation episode
    // used for an INSIDE -> OUTSIDE transition.
    startConfirmation(ConfirmationOrigin::kUnclassified, relation);
    return GeofenceOperationalResult::kConfirmationStarted;
  }

  if (confirmed_state_ == GeofenceOperationalState::kInside) {
    if (relation == PermittedAreaRelation::kInside) {
      return GeofenceOperationalResult::kStableInside;
    }
    if (relation == PermittedAreaRelation::kBoundary) {
      return GeofenceOperationalResult::kBoundaryNoDecision;
    }
    startConfirmation(ConfirmationOrigin::kInside, relation);
    return GeofenceOperationalResult::kConfirmationStarted;
  }

  if (relation == PermittedAreaRelation::kOutside) {
    return GeofenceOperationalResult::kStableOutside;
  }
  if (relation == PermittedAreaRelation::kBoundary) {
    return GeofenceOperationalResult::kBoundaryNoDecision;
  }
  startConfirmation(ConfirmationOrigin::kOutside, relation);
  return GeofenceOperationalResult::kConfirmationStarted;
}

bool GeofenceOperationalStateMachine::abortConfirmation() {
  if (!confirmation_active_) return false;
  clearConfirmation();
  return true;
}

void GeofenceOperationalStateMachine::reset() {
  has_confirmed_state_ = false;
  confirmed_state_ = GeofenceOperationalState::kInside;
  clearConfirmation();
}

}  // namespace orun_tlp
