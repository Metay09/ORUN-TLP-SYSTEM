#include "runtime_config.h"

namespace orun_tlp {
namespace {

ServiceStatus disabledStatus() {
  return ServiceStatus(ServiceState::kDisabled, ServiceReason::kNotRequested);
}

ServiceStatus enabledStatus() {
  return ServiceStatus(ServiceState::kEnabled, ServiceReason::kNone);
}

ServiceStatus blockedStatus(ServiceReason reason) {
  return ServiceStatus(ServiceState::kBlocked, reason);
}

ServiceStatus degradedStatus(ServiceReason reason) {
  return ServiceStatus(ServiceState::kDegraded, reason);
}

ServiceStatus resolveGnssTracking(const RequestedConfig& requested,
                                  const CapabilityState& gnss) {
  if (!requested.tracking_enabled) return disabledStatus();
  if (requested.location_source != RequestedLocationSource::kGnss)
    return blockedStatus(ServiceReason::kInvalidConfiguration);
  if (!gnss.supported)
    return blockedStatus(ServiceReason::kCapabilityUnsupported);

  switch (gnss.presence) {
    case CapabilityPresence::kUnknown:
      return blockedStatus(ServiceReason::kCapabilityUnknown);
    case CapabilityPresence::kAbsent:
      return blockedStatus(ServiceReason::kCapabilityAbsent);
    case CapabilityPresence::kPresent:
      break;
  }

  switch (gnss.health) {
    case CapabilityHealth::kOk:
      return enabledStatus();
    case CapabilityHealth::kDegraded:
      return degradedStatus(ServiceReason::kCapabilityDegraded);
    case CapabilityHealth::kFault:
      return blockedStatus(ServiceReason::kCapabilityFault);
    case CapabilityHealth::kUnavailable:
      return blockedStatus(ServiceReason::kCapabilityUnavailable);
  }
  return blockedStatus(ServiceReason::kCapabilityUnavailable);
}

}  // namespace

ConfigValidation validateRequestedConfig(const RequestedConfig& config) {
  if (config.tracking_enabled &&
      config.location_source == RequestedLocationSource::kNone) {
    return ConfigValidation::kTrackingRequiresLocationSource;
  }
  return ConfigValidation::kOk;
}

RequestedConfig requestedConfigFromLegacyBehavior(LegacyRoleBehavior behavior) {
  return RequestedConfig(
      behavior.publish_gnss_position, behavior.relay_forwarding_enabled,
      behavior.publish_gnss_position ? RequestedLocationSource::kGnss
                                     : RequestedLocationSource::kNone);
}

EffectiveConfig resolveRequestedConfig(const RequestedConfig& requested,
                                       const CapabilitySnapshot& capabilities) {
  // Invalid candidates are atomic: a future configuration owner must reject
  // them before replacing authoritative requested state. If an invalid value
  // reaches the pure resolver defensively, fail closed instead of partially
  // applying otherwise-valid services from the same candidate.
  if (validateRequestedConfig(requested) != ConfigValidation::kOk) {
    const ServiceStatus invalid =
        blockedStatus(ServiceReason::kInvalidConfiguration);
    return EffectiveConfig(invalid, invalid);
  }

  const ServiceStatus tracking =
      resolveGnssTracking(requested, capabilities.gnss);
  const ServiceStatus relay = requested.relay_forwarding_enabled
                                  ? enabledStatus()
                                  : disabledStatus();
  return EffectiveConfig(tracking, relay);
}

}  // namespace orun_tlp
