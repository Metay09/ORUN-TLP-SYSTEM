#pragma once

#include <stdint.h>

namespace orun_tlp {

// M7P7H: bounded transport-neutral read snapshots. These are application
// semantics, not wire frames and not domain owners. Production composition
// maps existing owner state into these PODs; ApplicationRequestService only
// snapshots/copies them. Keep this header free of Arduino/driver dependencies.

constexpr uint8_t kApplicationSurfaceRevision = 1;

enum class ApplicationRole : uint8_t {
  kTracker = 0,
  kRelay = 1,
  kBase = 2,
};

enum class ApplicationPresence : uint8_t {
  kUnknown = 0,
  kPresent = 1,
  kAbsent = 2,
};

enum class ApplicationHealth : uint8_t {
  kOk = 0,
  kDegraded = 1,
  kFault = 2,
  kUnavailable = 3,
};

enum class ApplicationServiceState : uint8_t {
  kDisabled = 0,
  kEnabled = 1,
  kBlocked = 2,
  kDegraded = 3,
};

enum class ApplicationServiceReason : uint8_t {
  kNone = 0,
  kNotRequested = 1,
  kInvalidConfiguration = 2,
  kCapabilityUnsupported = 3,
  kCapabilityUnknown = 4,
  kCapabilityAbsent = 5,
  kCapabilityDegraded = 6,
  kCapabilityFault = 7,
  kCapabilityUnavailable = 8,
};

enum class ApplicationGnssState : uint8_t {
  kNotPresent = 0,
  kDetectionBackoff = 1,
  kPowerOff = 2,
  kPowerOnWait = 3,
  kDetecting = 4,
  kIdle = 5,
  kStarting = 6,
  kAcquiring = 7,
  kFixAvailable = 8,
  kTimeout = 9,
  kFailure = 10,
  kSleeping = 11,
};

enum class ApplicationGeofenceResourceState : uint8_t {
  kUnavailable = 0,
  kClear = 1,
  kConfigured = 2,
};

enum class ApplicationGeofenceTokenState : uint8_t {
  kUnavailable = 0,
  kValid = 1,
  kUncertain = 2,
};

enum class ApplicationGeofenceOperationalState : uint8_t {
  kUnknown = 0,
  kInside = 1,
  kOutside = 2,
};

enum class ApplicationCadenceMode : uint8_t {
  kBase = 0,
  kBaseDividedBy3 = 1,
};

enum class ApplicationSecurityState : uint8_t {
  kUnprovisioned = 0,
  kProvisioned = 1,
  kForeign = 2,
  kUnsupported = 3,
  kFault = 4,
};

// DEVICE: compact product-facing identity/runtime/capability summary.
// Firmware version uses one bounded fixed field; verbose build metadata remains
// USB/service text rather than expanding the product wire surface.
constexpr uint8_t kApplicationFirmwareVersionSize = 16;

struct ApplicationDeviceSnapshot {
  uint32_t uptime_ms;
  uint32_t reset_reason;
  char firmware_version[kApplicationFirmwareVersionSize];
  uint8_t surface_revision;
  ApplicationRole role;
  uint8_t role_automatic;
  uint8_t watchdog_reset;
  ApplicationPresence gnss_presence;
  ApplicationHealth gnss_health;
  ApplicationPresence accelerometer_presence;
  ApplicationHealth accelerometer_health;
  ApplicationServiceState tracking_state;
  ApplicationServiceReason tracking_reason;
  ApplicationServiceState relay_state;
  ApplicationServiceReason relay_reason;
};

// TRACKING/GNSS: requested durable base B, actually applied base B and current
// runtime-effective cadence are deliberately distinct.
struct ApplicationTrackingSnapshot {
  uint32_t requested_interval_seconds;
  uint32_t applied_base_interval_seconds;
  uint32_t effective_interval_seconds;
  uint32_t acquisition_attempts;
  uint32_t successful_fresh_fixes;
  uint32_t acquisition_timeouts;
  uint32_t invalid_fixes;
  uint32_t last_ttff_ms;
  uint8_t config_backend_ready;
  uint8_t config_has_committed_record;
  uint8_t gnss_detected;
  uint8_t additional_fix_active;
  ApplicationCadenceMode cadence_mode;
  ApplicationGnssState gnss_state;
};

// GEOFENCE: summary only. Geometry and token bytes are intentionally absent.
struct ApplicationGeofenceSnapshot {
  uint16_t total_vertex_count;
  uint8_t area_count;
  ApplicationGeofenceResourceState resource_state;
  ApplicationGeofenceTokenState token_state;
  uint8_t runtime_configured;
  uint8_t has_confirmed_state;
  ApplicationGeofenceOperationalState confirmed_state;
  uint8_t confirmation_active;
  ApplicationCadenceMode cadence_mode;
};

// STORAGE: O(1) counters/state only. Do not populate this by scanning history.
struct ApplicationStorageSnapshot {
  uint32_t history_count;
  uint32_t history_capacity;
  uint32_t history_overwritten;
  uint32_t history_append_failures;
  uint32_t history_recovery_corruptions;
  uint32_t history_metadata_failures;
  uint32_t config_recovery_corruptions;
  uint32_t geofence_recovery_corruptions;
  uint32_t security_recovery_corruptions;
  uint8_t history_ready;
  uint8_t history_busy;
  uint8_t config_ready;
  uint8_t config_maintenance;
  uint8_t geofence_ready;
  uint8_t geofence_maintenance;
  uint8_t security_ready;
  uint8_t security_exhausted;
  ApplicationSecurityState security_state;
};

struct ApplicationStatusSnapshot {
  ApplicationDeviceSnapshot device;
  ApplicationTrackingSnapshot tracking;
  ApplicationGeofenceSnapshot geofence;
  ApplicationStorageSnapshot storage;
};

static_assert(sizeof(ApplicationDeviceSnapshot) <= 40,
              "DEVICE snapshot must remain bounded");
static_assert(sizeof(ApplicationTrackingSnapshot) <= 40,
              "TRACKING snapshot must remain bounded");
static_assert(sizeof(ApplicationGeofenceSnapshot) <= 16,
              "GEOFENCE snapshot must remain bounded");
static_assert(sizeof(ApplicationStorageSnapshot) <= 48,
              "STORAGE snapshot must remain bounded");

}  // namespace orun_tlp
