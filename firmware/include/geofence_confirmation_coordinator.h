#pragma once

#include <stdint.h>

#include "geofence_operational_state.h"
#include "geofence_runtime.h"
#include "geofence_runtime_policy.h"

namespace orun_tlp {

namespace geofence_confirmation_config {

// M6D2 field-validation seed. This bounds only the extra confirmation episode;
// every individual Location observation still has to satisfy the existing R3
// 5-second live-freshness contract before it reaches this class. GnssManager's
// original 120-second acquisition timeout remains the harder outer ceiling.
constexpr uint32_t kConfirmationDeadlineMs = 10000;

}  // namespace geofence_confirmation_config

struct GeofenceConfirmationUpdate {
  GeofenceConfirmationUpdate()
      : geometry_result(GeofenceObservationResult::kNotConfigured),
        operational_result(GeofenceOperationalResult::kInvalidObservation),
        request_additional_observation(false),
        episode_evidence_accepted(false),
        episode_slot(0),
        representative_available(false),
        representative_slot(0),
        outside_event_occurrence(false),
        cadence_changed(false),
        cadence_mode(GeofenceCadenceMode::kBase) {}

  GeofenceObservationResult geometry_result;
  GeofenceOperationalResult operational_result;
  bool request_additional_observation;
  bool episode_evidence_accepted;
  uint8_t episode_slot;
  bool representative_available;
  uint8_t representative_slot;
  // True only for a physical confirmed INSIDE -> OUTSIDE transition. Initial
  // discovery of OUTSIDE after boot/config replacement is deliberately false.
  bool outside_event_occurrence;
  bool cadence_changed;
  GeofenceCadenceMode cadence_mode;
};

// M6D2 source-neutral composition of M6C runtime geometry and the M6D1
// operational state machine. It consumes coordinates/timestamps/quality which
// the active Location owner has already accepted. It does not read GNSS parser
// internals, persist geometry/state, send RF, or own acquisition power.
class GeofenceConfirmationCoordinator {
 public:
  GeofenceRuntimeConfigResult configure(const GeofenceAreaSetView& candidate);
  void clear();

  bool configured() const { return runtime_.configured(); }
  bool confirmationActive() const { return operational_.confirmationActive(); }
  GeofenceCadenceMode cadenceMode() const { return operational_.cadenceMode(); }

  GeofenceConfirmationUpdate observeAcceptedLocation(
      const GeoPointE7& point, uint32_t captured_at_ms, uint16_t hdop_x100,
      uint8_t satellites);

  // Returns true only when an active episode was aborted at the exact bounded
  // deadline. The previously confirmed state/cadence remains authoritative.
  bool expireConfirmation(uint32_t now_ms);

  // Acquisition-owner failure/cancellation seam. Used when GnssManager cannot
  // continue the same accepted-fix session (timeout, I2C recovery, or explicit
  // continuation rejection). Prior confirmed authority/cadence is preserved.
  bool abortConfirmation();

 private:
  void clearEpisodeEvidence();
  void addEpisodeEvidence(PermittedAreaRelation relation, uint16_t hdop_x100,
                          uint8_t satellites);
  bool chooseRepresentative(GeofenceOperationalResult result,
                            uint8_t* representative_slot) const;

  GeofenceRuntime runtime_;
  GeofenceOperationalStateMachine operational_;
  geofence_runtime_policy::RepresentativeCandidate
      evidence_[geofence_operational_config::kConfirmationObservationLimit]{};
  uint8_t evidence_count_ = 0;
  uint32_t confirmation_started_at_ms_ = 0;
  bool confirmation_deadline_active_ = false;
};

}  // namespace orun_tlp
