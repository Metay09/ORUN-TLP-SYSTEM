#pragma once

#include <stdint.h>

#include "geofence_area_set.h"

namespace orun_tlp {

namespace geofence_operational_config {

constexpr uint8_t kConfirmationObservationLimit = 3;
constexpr uint8_t kTransitionVotesRequired = 2;

}  // namespace geofence_operational_config

enum class GeofenceOperationalState : uint8_t {
  kInside,
  kOutside,
};

enum class GeofenceCadenceMode : uint8_t {
  kBase,
  kBaseDividedBy3,
};

enum class GeofenceOperationalResult : uint8_t {
  kInvalidObservation,
  kBoundaryNoDecision,
  kInitializedInside,
  kInitializedOutside,
  kStableInside,
  kStableOutside,
  kConfirmationStarted,
  kConfirmationContinues,
  kConfirmationRejected,
  kConfirmedOutside,
  kConfirmedInside,
};

// M6D1 pure operational-state owner.
//
// Input is an already accepted geofence relation. This class does not own
// geometry, Location/GNSS freshness, acquisition scheduling, persistence,
// event transport or RF delivery.
//
// Product-visible confirmed states remain INSIDE/OUTSIDE only. Internally the
// machine may be unclassified after construction/reset/config replacement until
// fresh evidence establishes a state.
class GeofenceOperationalStateMachine {
 public:
  GeofenceOperationalStateMachine() = default;

  GeofenceOperationalResult observe(PermittedAreaRelation relation);

  bool hasConfirmedState() const { return has_confirmed_state_; }
  bool getConfirmedState(GeofenceOperationalState* state) const;

  bool confirmationActive() const { return confirmation_active_; }
  uint8_t confirmationObservationCount() const {
    return confirmation_observation_count_;
  }
  uint8_t confirmationInsideVotes() const { return confirmation_inside_votes_; }
  uint8_t confirmationOutsideVotes() const { return confirmation_outside_votes_; }

  // Called by the later timing/acquisition owner when a bounded confirmation
  // episode cannot be completed. The prior confirmed state, if any, remains
  // authoritative. No transition/event is invented.
  bool abortConfirmation();

  // Used on boot/runtime reset or after an authoritative area-set replacement
  // invalidates evidence collected against the old geometry. This is runtime
  // state only; M6D1 adds no persistence.
  void reset();

  GeofenceCadenceMode cadenceMode() const {
    return has_confirmed_state_ &&
                   confirmed_state_ == GeofenceOperationalState::kOutside
               ? GeofenceCadenceMode::kBaseDividedBy3
               : GeofenceCadenceMode::kBase;
  }

 private:
  enum class ConfirmationOrigin : uint8_t {
    kNone,
    kUnclassified,
    kInside,
    kOutside,
  };

  void startConfirmation(ConfirmationOrigin origin,
                         PermittedAreaRelation first_relation);
  void addConfirmationObservation(PermittedAreaRelation relation);
  GeofenceOperationalResult finishConfirmation();
  void clearConfirmation();

  bool has_confirmed_state_ = false;
  GeofenceOperationalState confirmed_state_ = GeofenceOperationalState::kInside;

  bool confirmation_active_ = false;
  ConfirmationOrigin confirmation_origin_ = ConfirmationOrigin::kNone;
  uint8_t confirmation_observation_count_ = 0;
  uint8_t confirmation_inside_votes_ = 0;
  uint8_t confirmation_outside_votes_ = 0;
};

}  // namespace orun_tlp
