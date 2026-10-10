#include "usb_application_adapter.h"

#include <Arduino.h>

#include <string.h>

#include "config_format.h"
#include "service_intent.h"

namespace orun_tlp {
namespace {

bool equals(const char* text, uint8_t length, const char* expected) {
  if (text == nullptr || expected == nullptr) return false;
  const size_t expected_length = strlen(expected);
  return expected_length == length &&
         memcmp(text, expected, expected_length) == 0;
}

}  // namespace

bool parseUsbApplicationQuery(const char* text, uint8_t length,
                              ApplicationRequestKind* kind) {
  if (kind == nullptr) return false;

  if (equals(text, length, "APP CONFIG?")) {
    *kind = ApplicationRequestKind::kGetConfig;
    return true;
  }
  if (equals(text, length, "APP DEVICE?")) {
    *kind = ApplicationRequestKind::kGetDeviceStatus;
    return true;
  }
  if (equals(text, length, "APP TRACKING?")) {
    *kind = ApplicationRequestKind::kGetTrackingStatus;
    return true;
  }
  if (equals(text, length, "APP GEOFENCE?")) {
    *kind = ApplicationRequestKind::kGetGeofenceStatus;
    return true;
  }
  if (equals(text, length, "APP STORAGE?")) {
    *kind = ApplicationRequestKind::kGetStorageStatus;
    return true;
  }
  return false;
}

UsbConfigCommandParse parseUsbConfigCommand(const char* text, uint8_t length,
                                            ConfigMutationKind* kind,
                                            uint32_t* value) {
  static const char kPrefix[] = "APP INTERVAL ";
  constexpr uint8_t kPrefixLength = sizeof(kPrefix) - 1;
  if (text == nullptr || kind == nullptr || value == nullptr ||
      length < kPrefixLength || memcmp(text, kPrefix, kPrefixLength) != 0) {
    return UsbConfigCommandParse::kNotConfigCommand;
  }

  // Strict unsigned decimal: at least one digit, nothing else, no overflow.
  if (length == kPrefixLength) return UsbConfigCommandParse::kMalformed;
  uint32_t parsed = 0;
  for (uint8_t index = kPrefixLength; index < length; ++index) {
    const char digit = text[index];
    if (digit < '0' || digit > '9') return UsbConfigCommandParse::kMalformed;
    const uint32_t increment = static_cast<uint32_t>(digit - '0');
    if (parsed > (UINT32_MAX - increment) / 10U)
      return UsbConfigCommandParse::kMalformed;
    parsed = parsed * 10U + increment;
  }

  *kind = ConfigMutationKind::kSetTrackingInterval;
  *value = parsed;
  return UsbConfigCommandParse::kOk;
}

UsbConfigCommandParse parseUsbServicesCommand(const char* text, uint8_t length,
                                              uint8_t* service_mode,
                                              uint8_t* requested_services) {
  static const char kPrefix[] = "APP SERVICES ";
  constexpr uint8_t kPrefixLength = sizeof(kPrefix) - 1;
  if (text == nullptr || service_mode == nullptr ||
      requested_services == nullptr || length < kPrefixLength ||
      memcmp(text, kPrefix, kPrefixLength) != 0) {
    return UsbConfigCommandParse::kNotConfigCommand;
  }
  uint8_t mode = 0;
  uint8_t services = 0;
  if (!parseServiceIntent(text + kPrefixLength,
                          static_cast<size_t>(length - kPrefixLength), &mode,
                          &services))
    return UsbConfigCommandParse::kMalformed;
  *service_mode = mode;
  *requested_services = services;
  return UsbConfigCommandParse::kOk;
}

bool isUsbServicesQuery(const char* text, uint8_t length) {
  return equals(text, length, "APP SERVICES?");
}

namespace {

const char* serviceStateName(const ServiceStatus& status) {
  switch (status.state) {
    case ServiceState::kDisabled: return "OFF";
    case ServiceState::kEnabled: return "ON";
    case ServiceState::kDegraded: return "DEGRADED";
    case ServiceState::kBlocked: break;
  }
  switch (status.reason) {
    case ServiceReason::kCapabilityAbsent: return "BLOCKED_GNSS_ABSENT";
    case ServiceReason::kCapabilityUnknown: return "BLOCKED_GNSS_PENDING";
    case ServiceReason::kCapabilityFault: return "BLOCKED_GNSS_FAULT";
    case ServiceReason::kCapabilityUnavailable: return "BLOCKED_GNSS_UNAVAILABLE";
    case ServiceReason::kCapabilityUnsupported: return "BLOCKED_UNSUPPORTED";
    case ServiceReason::kInvalidConfiguration: return "BLOCKED_INVALID";
    default: return "BLOCKED";
  }
}

}  // namespace

void printUsbServiceStatus(const UsbServiceStatus& status) {
  char requested[5];
  formatServices(status.requested_services, requested, sizeof(requested));
  const bool explicit_mode =
      status.service_mode == config_format::kServiceModeExplicit;
  const char* applied = "LEGACY_AUTO";
  if (explicit_mode)
    applied = status.intent_applied ? "EXPLICIT" : "LEGACY_AUTO_UNSUPPORTED";
  Serial.printf(
      "APP SERVICES mode=%s requested=%s applied=%s tracking=%s relay=%s "
      "receive=%s legacy_mode=%s\n",
      explicit_mode ? "EXPLICIT" : "AUTO", explicit_mode ? requested : "-",
      applied, serviceStateName(status.tracking),
      serviceStateName(status.relay_forwarding),
      status.application_receive ? "ON" : "OFF", roleName(status.legacy_role));
}

void printUsbConfigMutationResult(const ConfigMutationResult& result) {
  const char* code = "UNAVAILABLE";
  switch (result.outcome) {
    case ConfigMutationOutcome::kApplied: code = "APPLIED"; break;
    case ConfigMutationOutcome::kUnchanged: code = "UNCHANGED"; break;
    case ConfigMutationOutcome::kInvalid: code = "INVALID"; break;
    case ConfigMutationOutcome::kAccessDenied: code = "ACCESS_DENIED"; break;
    case ConfigMutationOutcome::kMaintenance: code = "MAINTENANCE"; break;
    case ConfigMutationOutcome::kUnavailable: code = "UNAVAILABLE"; break;
    case ConfigMutationOutcome::kOutcomeUnknown: code = "OUTCOME_UNKNOWN"; break;
  }

  // What is printed is what the store holds now, never the requested value:
  // after OUTCOME_UNKNOWN or a refusal it is the previous setting.
  if (result.kind == ConfigMutationKind::kSetServiceIntent) {
    char services[5];
    const char* mode =
        result.config.service_mode == config_format::kServiceModeExplicit
            ? "EXPLICIT"
            : "AUTO";
    formatServices(result.config.requested_services, services,
                   sizeof(services));
    if (result.token_valid) {
      Serial.printf("APP SET id=%lu code=%s services=%s mode=%s "
                    "token=VALID revision=%lu\n",
                    static_cast<unsigned long>(result.request_id), code,
                    result.config.service_mode ==
                            config_format::kServiceModeExplicit
                        ? services
                        : "AUTO",
                    mode, static_cast<unsigned long>(result.token.revision));
    } else {
      Serial.printf("APP SET id=%lu code=%s services=%s mode=%s "
                    "token=UNAVAILABLE\n",
                    static_cast<unsigned long>(result.request_id), code,
                    result.config.service_mode ==
                            config_format::kServiceModeExplicit
                        ? services
                        : "AUTO",
                    mode);
    }
    return;
  }

  if (result.token_valid) {
    Serial.printf(
        "APP SET id=%lu code=%s tracking_interval_seconds=%lu "
        "token=VALID revision=%lu\n",
        static_cast<unsigned long>(result.request_id), code,
        static_cast<unsigned long>(result.config.tracking_interval_seconds),
        static_cast<unsigned long>(result.token.revision));
  } else {
    Serial.printf(
        "APP SET id=%lu code=%s tracking_interval_seconds=%lu "
        "token=UNAVAILABLE\n",
        static_cast<unsigned long>(result.request_id), code,
        static_cast<unsigned long>(result.config.tracking_interval_seconds));
  }
}

// Adafruit nRF52 1.7.0 Print::printf formats into a 256-byte stack buffer and
// then writes the UNTRUNCATED length. A status line longer than 255 characters
// therefore loses its tail and prints stack bytes instead (observed on a
// RAK4631: APP STORAGE ended in binary garbage, APP DEVICE lost its newline).
// Every printf call below must stay under that limit for all field values, so
// the long status lines are emitted as several calls that form one line.
void printUsbApplicationResponse(const ApplicationResponse& response) {
  if (response.code != ApplicationResponseCode::kOk) {
    const char* code = "UNSUPPORTED";
    if (response.code == ApplicationResponseCode::kUnavailable)
      code = "UNAVAILABLE";
    else if (response.code == ApplicationResponseCode::kAccessDenied)
      code = "ACCESS_DENIED";
    Serial.printf("APP RESULT id=%lu code=%s\n",
                  static_cast<unsigned long>(response.request_id), code);
    return;
  }

  switch (response.kind) {
    case ApplicationRequestKind::kGetConfig: {
      const ApplicationConfigResponsePayload& p = response.payload.config;
      const char* source = p.has_committed_record != 0 ? "stored" : "default";
      // Preserve M7P7D's established USB GET_CONFIG output.
      Serial.printf(
          "APP RESULT id=%lu code=OK config_backend_ready=%s source=%s "
          "tracking_interval_seconds=%lu battery_capacity_mah=%lu\n",
          static_cast<unsigned long>(response.request_id),
          p.backend_ready != 0 ? "yes" : "no", source,
          static_cast<unsigned long>(p.tracking_interval_seconds),
          static_cast<unsigned long>(p.battery_capacity_mah));
      return;
    }

    case ApplicationRequestKind::kGetDeviceStatus: {
      const ApplicationDeviceSnapshot& s = response.payload.device.snapshot;
      // One status line, several printf calls: see the note above
      // printUsbApplicationResponse(). Output bytes are unchanged.
      Serial.printf(
          "APP DEVICE id=%lu fw=%s rev=%u uptime_ms_mod32=%lu reset=0x%08lX watchdog=%s "
          "role=%u role_mode=%s relay_applied=%s ",
          static_cast<unsigned long>(response.request_id),
          s.firmware_version,
          static_cast<unsigned>(s.surface_revision),
          static_cast<unsigned long>(s.uptime_ms_mod32),
          static_cast<unsigned long>(s.reset_reason),
          s.watchdog_reset ? "yes" : "no",
          static_cast<unsigned>(s.role),
          s.role_automatic ? "AUTO" : "OVERRIDE",
          s.relay_forwarding_applied ? "yes" : "no");
      Serial.printf(
          "gnss_presence=%u gnss_health=%u "
          "accel_presence=%u accel_health=%u tracking_state=%u "
          "tracking_reason=%u relay_state=%u relay_reason=%u\n",
          static_cast<unsigned>(s.gnss_presence),
          static_cast<unsigned>(s.gnss_health),
          static_cast<unsigned>(s.accelerometer_presence),
          static_cast<unsigned>(s.accelerometer_health),
          static_cast<unsigned>(s.tracking_state),
          static_cast<unsigned>(s.tracking_reason),
          static_cast<unsigned>(s.relay_state),
          static_cast<unsigned>(s.relay_reason));
      return;
    }

    case ApplicationRequestKind::kGetTrackingStatus: {
      const ApplicationTrackingSnapshot& s =
          response.payload.tracking.snapshot;
      Serial.printf(
          "APP TRACKING id=%lu requested_s=%lu applied_s=%lu effective_s=%lu "
          "cadence=%u gnss_state=%u detected=%s extra_fix=%s ",
          static_cast<unsigned long>(response.request_id),
          static_cast<unsigned long>(s.requested_interval_seconds),
          static_cast<unsigned long>(s.applied_base_interval_seconds),
          static_cast<unsigned long>(s.effective_interval_seconds),
          static_cast<unsigned>(s.cadence_mode),
          static_cast<unsigned>(s.gnss_state),
          s.gnss_detected ? "yes" : "no",
          s.additional_fix_active ? "yes" : "no");
      Serial.printf(
          "attempts=%lu fresh=%lu timeouts=%lu invalid=%lu last_ttff_ms=%lu "
          "config_ready=%s source=%s\n",
          static_cast<unsigned long>(s.acquisition_attempts),
          static_cast<unsigned long>(s.successful_fresh_fixes),
          static_cast<unsigned long>(s.acquisition_timeouts),
          static_cast<unsigned long>(s.invalid_fixes),
          static_cast<unsigned long>(s.last_ttff_ms),
          s.config_backend_ready ? "yes" : "no",
          s.config_has_committed_record ? "stored" : "default");
      return;
    }

    case ApplicationRequestKind::kGetGeofenceStatus: {
      const ApplicationGeofenceSnapshot& s =
          response.payload.geofence.snapshot;
      Serial.printf(
          "APP GEOFENCE id=%lu resource=%u token=%u areas=%u vertices=%u "
          "runtime=%s confirmed=%s state=%u confirmation=%s cadence=%u\n",
          static_cast<unsigned long>(response.request_id),
          static_cast<unsigned>(s.resource_state),
          static_cast<unsigned>(s.token_state),
          static_cast<unsigned>(s.area_count),
          static_cast<unsigned>(s.total_vertex_count),
          s.runtime_configured ? "configured" : "clear",
          s.has_confirmed_state ? "yes" : "no",
          static_cast<unsigned>(s.confirmed_state),
          s.confirmation_active ? "active" : "idle",
          static_cast<unsigned>(s.cadence_mode));
      return;
    }

    case ApplicationRequestKind::kGetStorageStatus: {
      const ApplicationStorageSnapshot& s = response.payload.storage.snapshot;
      Serial.printf(
          "APP STORAGE id=%lu history_ready=%s busy=%s count=%lu capacity=%lu "
          "overwritten=%lu append_failures=%lu corrupt=%lu metadata_failures=%lu ",
          static_cast<unsigned long>(response.request_id),
          s.history_ready ? "yes" : "no",
          s.history_busy ? "yes" : "no",
          static_cast<unsigned long>(s.history_count),
          static_cast<unsigned long>(s.history_capacity),
          static_cast<unsigned long>(s.history_overwritten),
          static_cast<unsigned long>(s.history_append_failures),
          static_cast<unsigned long>(s.history_recovery_corruptions),
          static_cast<unsigned long>(s.history_metadata_failures));
      Serial.printf(
          "config_ready=%s config_maintenance=%s config_corrupt=%lu "
          "geofence_ready=%s geofence_maintenance=%s geofence_corrupt=%lu ",
          s.config_ready ? "yes" : "no",
          s.config_maintenance ? "yes" : "no",
          static_cast<unsigned long>(s.config_recovery_corruptions),
          s.geofence_ready ? "yes" : "no",
          s.geofence_maintenance ? "yes" : "no",
          static_cast<unsigned long>(s.geofence_recovery_corruptions));
      Serial.printf(
          "security_ready=%s security_state=%u security_exhausted=%s "
          "security_corrupt=%lu\n",
          s.security_ready ? "yes" : "no",
          static_cast<unsigned>(s.security_state),
          s.security_exhausted ? "yes" : "no",
          static_cast<unsigned long>(s.security_recovery_corruptions));
      return;
    }
  }
}

}  // namespace orun_tlp
