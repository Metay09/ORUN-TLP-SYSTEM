#pragma once

#include <stdint.h>

#include "node_behavior.h"

namespace orun_tlp {

enum class CapabilityPresence : uint8_t {
  kUnknown,
  kPresent,
  kAbsent,
};

enum class CapabilityHealth : uint8_t {
  kOk,
  kDegraded,
  kFault,
  kUnavailable,
};

struct CapabilityState {
  constexpr CapabilityState(bool supported_value = false,
                            CapabilityPresence presence_value =
                                CapabilityPresence::kUnknown,
                            CapabilityHealth health_value =
                                CapabilityHealth::kUnavailable)
      : supported(supported_value),
        presence(presence_value),
        health(health_value) {}

  bool supported;
  CapabilityPresence presence;
  CapabilityHealth health;
};

struct CapabilitySnapshot {
  constexpr CapabilitySnapshot(
      CapabilityState gnss_value = CapabilityState(),
      CapabilityState accelerometer_value = CapabilityState())
      : gnss(gnss_value), accelerometer(accelerometer_value) {}

  CapabilityState gnss;
  CapabilityState accelerometer;
};

enum class RequestedLocationSource : uint8_t {
  kNone,
  kGnss,
};

struct RequestedConfig {
  constexpr RequestedConfig(bool tracking_enabled_value = false,
                            bool relay_forwarding_enabled_value = false,
                            RequestedLocationSource location_source_value =
                                RequestedLocationSource::kNone)
      : tracking_enabled(tracking_enabled_value),
        relay_forwarding_enabled(relay_forwarding_enabled_value),
        location_source(location_source_value) {}

  bool tracking_enabled;
  bool relay_forwarding_enabled;
  RequestedLocationSource location_source;
};

enum class ConfigValidation : uint8_t {
  kOk,
  kTrackingRequiresLocationSource,
  kInvalidLocationSource,
};

enum class ServiceState : uint8_t {
  kDisabled,
  kEnabled,
  kBlocked,
  kDegraded,
};

enum class ServiceReason : uint8_t {
  kNone,
  kNotRequested,
  kInvalidConfiguration,
  kCapabilityUnsupported,
  kCapabilityUnknown,
  kCapabilityAbsent,
  kCapabilityDegraded,
  kCapabilityFault,
  kCapabilityUnavailable,
};

struct ServiceStatus {
  constexpr ServiceStatus(ServiceState state_value = ServiceState::kDisabled,
                          ServiceReason reason_value =
                              ServiceReason::kNotRequested)
      : state(state_value), reason(reason_value) {}

  ServiceState state;
  ServiceReason reason;
};

struct EffectiveConfig {
  constexpr EffectiveConfig(ServiceStatus tracking_value = ServiceStatus(),
                            ServiceStatus relay_forwarding_value =
                                ServiceStatus())
      : tracking(tracking_value), relay_forwarding(relay_forwarding_value) {}

  ServiceStatus tracking;
  ServiceStatus relay_forwarding;
};

ConfigValidation validateRequestedConfig(const RequestedConfig& config);

RequestedConfig requestedConfigFromLegacyBehavior(LegacyRoleBehavior behavior);

EffectiveConfig resolveRequestedConfig(const RequestedConfig& requested,
                                       const CapabilitySnapshot& capabilities);

}  // namespace orun_tlp
