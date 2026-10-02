#include "application_status_runtime.h"

#include "geofence_runtime_policy.h"
#include "watchdog_manager.h"

namespace orun_tlp {
namespace {

ApplicationRole mapRole(NodeRole role) {
  switch (role) {
    case NodeRole::kTracker: return ApplicationRole::kTracker;
    case NodeRole::kRelay: return ApplicationRole::kRelay;
    case NodeRole::kBase: return ApplicationRole::kBase;
  }
  return ApplicationRole::kBase;
}

ApplicationPresence mapPresence(CapabilityPresence presence) {
  switch (presence) {
    case CapabilityPresence::kUnknown: return ApplicationPresence::kUnknown;
    case CapabilityPresence::kPresent: return ApplicationPresence::kPresent;
    case CapabilityPresence::kAbsent: return ApplicationPresence::kAbsent;
  }
  return ApplicationPresence::kUnknown;
}

ApplicationHealth mapHealth(CapabilityHealth health) {
  switch (health) {
    case CapabilityHealth::kOk: return ApplicationHealth::kOk;
    case CapabilityHealth::kDegraded: return ApplicationHealth::kDegraded;
    case CapabilityHealth::kFault: return ApplicationHealth::kFault;
    case CapabilityHealth::kUnavailable: return ApplicationHealth::kUnavailable;
  }
  return ApplicationHealth::kUnavailable;
}

ApplicationServiceState mapServiceState(ServiceState state) {
  switch (state) {
    case ServiceState::kDisabled: return ApplicationServiceState::kDisabled;
    case ServiceState::kEnabled: return ApplicationServiceState::kEnabled;
    case ServiceState::kBlocked: return ApplicationServiceState::kBlocked;
    case ServiceState::kDegraded: return ApplicationServiceState::kDegraded;
  }
  return ApplicationServiceState::kDisabled;
}

ApplicationServiceReason mapServiceReason(ServiceReason reason) {
  switch (reason) {
    case ServiceReason::kNone: return ApplicationServiceReason::kNone;
    case ServiceReason::kNotRequested:
      return ApplicationServiceReason::kNotRequested;
    case ServiceReason::kInvalidConfiguration:
      return ApplicationServiceReason::kInvalidConfiguration;
    case ServiceReason::kCapabilityUnsupported:
      return ApplicationServiceReason::kCapabilityUnsupported;
    case ServiceReason::kCapabilityUnknown:
      return ApplicationServiceReason::kCapabilityUnknown;
    case ServiceReason::kCapabilityAbsent:
      return ApplicationServiceReason::kCapabilityAbsent;
    case ServiceReason::kCapabilityDegraded:
      return ApplicationServiceReason::kCapabilityDegraded;
    case ServiceReason::kCapabilityFault:
      return ApplicationServiceReason::kCapabilityFault;
    case ServiceReason::kCapabilityUnavailable:
      return ApplicationServiceReason::kCapabilityUnavailable;
  }
  return ApplicationServiceReason::kInvalidConfiguration;
}

ApplicationGnssState mapGnssState(GnssManager::State state) {
  using Input = GnssManager::State;
  using Output = ApplicationGnssState;
  switch (state) {
    case Input::kNotPresent: return Output::kNotPresent;
    case Input::kDetectionBackoff: return Output::kDetectionBackoff;
    case Input::kPowerOff: return Output::kPowerOff;
    case Input::kPowerOnWait: return Output::kPowerOnWait;
    case Input::kDetecting: return Output::kDetecting;
    case Input::kIdle: return Output::kIdle;
    case Input::kStarting: return Output::kStarting;
    case Input::kAcquiring: return Output::kAcquiring;
    case Input::kFixAvailable: return Output::kFixAvailable;
    case Input::kTimeout: return Output::kTimeout;
    case Input::kFailure: return Output::kFailure;
    case Input::kSleeping: return Output::kSleeping;
  }
  return Output::kFailure;
}

ApplicationCadenceMode mapCadence(GeofenceCadenceMode mode) {
  return mode == GeofenceCadenceMode::kBaseDividedBy3
             ? ApplicationCadenceMode::kBaseDividedBy3
             : ApplicationCadenceMode::kBase;
}

ApplicationGeofenceResourceState mapResourceState(
    GeofenceResourceState state) {
  switch (state) {
    case GeofenceResourceState::kUnavailable:
      return ApplicationGeofenceResourceState::kUnavailable;
    case GeofenceResourceState::kClear:
      return ApplicationGeofenceResourceState::kClear;
    case GeofenceResourceState::kConfigured:
      return ApplicationGeofenceResourceState::kConfigured;
  }
  return ApplicationGeofenceResourceState::kUnavailable;
}

ApplicationGeofenceTokenState mapTokenState(GeofenceTokenState state) {
  switch (state) {
    case GeofenceTokenState::kUnavailable:
      return ApplicationGeofenceTokenState::kUnavailable;
    case GeofenceTokenState::kValid:
      return ApplicationGeofenceTokenState::kValid;
    case GeofenceTokenState::kUncertain:
      return ApplicationGeofenceTokenState::kUncertain;
  }
  return ApplicationGeofenceTokenState::kUnavailable;
}

ApplicationSecurityState mapSecurityState(SecurityState state) {
  switch (state) {
    case SecurityState::kUnprovisioned:
      return ApplicationSecurityState::kUnprovisioned;
    case SecurityState::kProvisioned:
      return ApplicationSecurityState::kProvisioned;
    case SecurityState::kForeign:
      return ApplicationSecurityState::kForeign;
    case SecurityState::kUnsupported:
      return ApplicationSecurityState::kUnsupported;
    case SecurityState::kFault:
      return ApplicationSecurityState::kFault;
  }
  return ApplicationSecurityState::kFault;
}

}  // namespace

void buildApplicationStatusSnapshot(
    uint32_t now_ms,
    NodeRole role,
    bool role_automatic,
    const CapabilitySnapshot& capabilities,
    const EffectiveConfig& effective,
    uint32_t applied_base_interval_seconds,
    const GnssManager& gnss,
    const ConfigStore& config,
    const GeofenceStore& geofence_store,
    const GeofenceConfirmationCoordinator& geofence_runtime,
    const HistoryStore& history,
    const SecurityStore& security,
    ApplicationStatusSnapshot& out) {
  const WatchdogManager::BootInfo& reset = WatchdogManager::bootInfo();

  ApplicationDeviceSnapshot& device = out.device;
  device.uptime_ms = now_ms;
  device.reset_reason = reset.reset_reason;
  device.surface_revision = kApplicationSurfaceRevision;
  device.role = mapRole(role);
  device.role_automatic = role_automatic ? 1U : 0U;
  device.watchdog_reset = reset.watchdog_reset ? 1U : 0U;
  device.gnss_presence = mapPresence(capabilities.gnss.presence);
  device.gnss_health = mapHealth(capabilities.gnss.health);
  device.accelerometer_presence =
      mapPresence(capabilities.accelerometer.presence);
  device.accelerometer_health =
      mapHealth(capabilities.accelerometer.health);
  device.tracking_state = mapServiceState(effective.tracking.state);
  device.tracking_reason = mapServiceReason(effective.tracking.reason);
  device.relay_state = mapServiceState(effective.relay_forwarding.state);
  device.relay_reason = mapServiceReason(effective.relay_forwarding.reason);

  const GnssManager::Diagnostics& gnss_diagnostics = gnss.diagnostics();
  ApplicationTrackingSnapshot& tracking = out.tracking;
  tracking.requested_interval_seconds =
      config.config().tracking_interval_seconds;
  tracking.applied_base_interval_seconds = applied_base_interval_seconds;
  const GeofenceCadenceMode cadence = geofence_runtime.cadenceMode();
  tracking.effective_interval_seconds =
      geofence_runtime_policy::effectiveTrackingIntervalMs(
          applied_base_interval_seconds, cadence) /
      1000UL;
  tracking.acquisition_attempts = gnss_diagnostics.acquisition_attempts;
  tracking.successful_fresh_fixes =
      gnss_diagnostics.successful_fresh_fixes;
  tracking.acquisition_timeouts = gnss_diagnostics.acquisition_timeouts;
  tracking.invalid_fixes = gnss_diagnostics.invalid_fixes;
  tracking.last_ttff_ms = gnss_diagnostics.last_ttff_ms;
  tracking.config_backend_ready = config.ready() ? 1U : 0U;
  tracking.config_has_committed_record =
      config.hasCommittedRecord() ? 1U : 0U;
  tracking.gnss_detected = gnss.detected() ? 1U : 0U;
  tracking.additional_fix_active =
      gnss.additionalFixAcquisitionActive() ? 1U : 0U;
  tracking.cadence_mode = mapCadence(cadence);
  tracking.gnss_state = mapGnssState(gnss.state());

  ApplicationGeofenceSnapshot& geofence = out.geofence;
  geofence.total_vertex_count = geofence_store.totalVertexCount();
  geofence.area_count = geofence_store.areaCount();
  geofence.resource_state = mapResourceState(geofence_store.resourceState());
  geofence.token_state = mapTokenState(geofence_store.tokenState());
  geofence.runtime_configured = geofence_runtime.configured() ? 1U : 0U;
  geofence.confirmation_active =
      geofence_runtime.confirmationActive() ? 1U : 0U;
  geofence.cadence_mode = mapCadence(geofence_runtime.cadenceMode());
  GeofenceOperationalState confirmed_state;
  geofence.has_confirmed_state =
      geofence_runtime.getConfirmedState(&confirmed_state) ? 1U : 0U;
  geofence.confirmed_state =
      geofence.has_confirmed_state == 0
          ? ApplicationGeofenceOperationalState::kUnknown
          : confirmed_state == GeofenceOperationalState::kOutside
                ? ApplicationGeofenceOperationalState::kOutside
                : ApplicationGeofenceOperationalState::kInside;

  const HistoryStore::Diagnostics& history_diagnostics =
      history.diagnostics();
  const ConfigStore::Diagnostics& config_diagnostics = config.diagnostics();
  const GeofenceStore::Diagnostics& geofence_diagnostics =
      geofence_store.diagnostics();
  const SecurityStore::Diagnostics& security_diagnostics =
      security.diagnostics();

  ApplicationStorageSnapshot& storage = out.storage;
  storage.history_count = history.count();
  storage.history_capacity = history.capacity();
  storage.history_overwritten = history_diagnostics.overwritten;
  storage.history_append_failures = history_diagnostics.append_failures;
  storage.history_recovery_corruptions =
      history_diagnostics.recovery_corruptions;
  storage.history_metadata_failures = history_diagnostics.metadata_failures;
  storage.config_recovery_corruptions =
      config_diagnostics.recovery_corruptions;
  storage.geofence_recovery_corruptions =
      geofence_diagnostics.recovery_corruptions;
  storage.security_recovery_corruptions =
      security_diagnostics.recovery_corruptions;
  storage.history_ready = history.ready() ? 1U : 0U;
  storage.history_busy = history.busy() ? 1U : 0U;
  storage.config_ready = config.ready() ? 1U : 0U;
  storage.config_maintenance =
      config.maintenanceResetRequired() ? 1U : 0U;
  storage.geofence_ready = geofence_store.ready() ? 1U : 0U;
  storage.geofence_maintenance =
      geofence_store.maintenanceResetRequired() ? 1U : 0U;
  storage.security_ready = security.ready() ? 1U : 0U;
  storage.security_exhausted = security.exhausted() ? 1U : 0U;
  storage.security_state = mapSecurityState(security.state());
}

}  // namespace orun_tlp
