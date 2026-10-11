#include "usb_application_adapter.h"

#include <Arduino.h>

#include <stdio.h>
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
  if (equals(text, length, "APP POWER?")) {
    *kind = ApplicationRequestKind::kGetPowerStatus;
    return true;
  }
  if (equals(text, length, "APP SERVICES?")) {
    *kind = ApplicationRequestKind::kGetServicesStatus;
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

namespace {

const char* serviceStateName(ApplicationServiceState state,
                             ApplicationServiceReason reason) {
  switch (state) {
    case ApplicationServiceState::kDisabled: return "OFF";
    case ApplicationServiceState::kEnabled: return "ON";
    case ApplicationServiceState::kDegraded: return "DEGRADED";
    case ApplicationServiceState::kBlocked: break;
  }
  switch (reason) {
    case ApplicationServiceReason::kCapabilityAbsent: return "BLOCKED_GNSS_ABSENT";
    case ApplicationServiceReason::kCapabilityUnknown: return "BLOCKED_GNSS_PENDING";
    case ApplicationServiceReason::kCapabilityFault: return "BLOCKED_GNSS_FAULT";
    case ApplicationServiceReason::kCapabilityUnavailable:
      return "BLOCKED_GNSS_UNAVAILABLE";
    case ApplicationServiceReason::kCapabilityUnsupported:
      return "BLOCKED_UNSUPPORTED";
    case ApplicationServiceReason::kInvalidConfiguration:
      return "BLOCKED_INVALID";
    default: return "BLOCKED";
  }
}

const char* applicationRoleName(ApplicationRole role) {
  switch (role) {
    case ApplicationRole::kTracker: return "TRACKER";
    case ApplicationRole::kRelay: return "RELAY";
    case ApplicationRole::kBase: return "BASE";
  }
  return "BASE";
}

const char* batteryStateName(ApplicationBatteryState state) {
  switch (state) {
    case ApplicationBatteryState::kUnknown: return "UNKNOWN";
    case ApplicationBatteryState::kNormal: return "NORMAL";
    case ApplicationBatteryState::kLow: return "LOW";
    case ApplicationBatteryState::kCritical: return "CRITICAL";
  }
  return "UNKNOWN";
}

}  // namespace

UsbFenceCommand parseUsbFenceCommand(const char* text, uint8_t length,
                                     GeoPointE7* vertex) {
  static const char kPrefix[] = "APP FENCE ";
  constexpr uint8_t kPrefixLength = sizeof(kPrefix) - 1;
  if (text == nullptr || length < kPrefixLength ||
      memcmp(text, kPrefix, kPrefixLength) != 0)
    return UsbFenceCommand::kNotFenceCommand;
  const char* arg = text + kPrefixLength;
  const uint8_t arg_length = static_cast<uint8_t>(length - kPrefixLength);
  if (equals(arg, arg_length, "BEGIN")) return UsbFenceCommand::kBegin;
  if (equals(arg, arg_length, "AREA")) return UsbFenceCommand::kArea;
  if (equals(arg, arg_length, "COMMIT")) return UsbFenceCommand::kCommit;
  if (equals(arg, arg_length, "ABORT")) return UsbFenceCommand::kAbort;
  if (equals(arg, arg_length, "CLEAR")) return UsbFenceCommand::kClear;

  // V <lat> <lon>: exactly one space between the parts.
  if (vertex == nullptr || arg_length < 2 || arg[0] != 'V' || arg[1] != ' ')
    return UsbFenceCommand::kMalformed;
  const char* lat = arg + 2;
  const uint8_t rest = static_cast<uint8_t>(arg_length - 2);
  const char* space = static_cast<const char*>(memchr(lat, ' ', rest));
  if (space == nullptr) return UsbFenceCommand::kMalformed;
  const size_t lat_length = static_cast<size_t>(space - lat);
  const char* lon = space + 1;
  const size_t lon_length = rest - lat_length - 1;
  int32_t lat_e7 = 0;
  int32_t lon_e7 = 0;
  if (!parseDegreesE7(lat, lat_length, 90, &lat_e7) ||
      !parseDegreesE7(lon, lon_length, 180, &lon_e7))
    return UsbFenceCommand::kMalformed;
  *vertex = GeoPointE7(lat_e7, lon_e7);
  return UsbFenceCommand::kVertex;
}

void printUsbFenceStaging(const GeofenceStaging& staging) {
  Serial.printf("APP FENCE STAGING areas=%u vertices=%u\n",
                static_cast<unsigned>(staging.areaCount()),
                static_cast<unsigned>(staging.vertexCount()));
}

void printUsbFenceRejected(GeofenceStaging::Result reason) {
  const char* text = "UNKNOWN";
  switch (reason) {
    case GeofenceStaging::Result::kOk: text = "NONE"; break;
    case GeofenceStaging::Result::kNotStaging: text = "NOT_STAGING"; break;
    case GeofenceStaging::Result::kFull: text = "FULL"; break;
    case GeofenceStaging::Result::kEmptyArea: text = "EMPTY_AREA"; break;
    case GeofenceStaging::Result::kTooManyAreas: text = "TOO_MANY_AREAS"; break;
  }
  Serial.printf("APP FENCE REJECTED reason=%s\n", text);
}

namespace {

const char* mutationCodeName(ConfigMutationOutcome outcome) {
  switch (outcome) {
    case ConfigMutationOutcome::kApplied: return "APPLIED";
    case ConfigMutationOutcome::kUnchanged: return "UNCHANGED";
    case ConfigMutationOutcome::kInvalid: return "INVALID";
    case ConfigMutationOutcome::kAccessDenied: return "ACCESS_DENIED";
    case ConfigMutationOutcome::kMaintenance: return "MAINTENANCE";
    case ConfigMutationOutcome::kUnavailable: return "UNAVAILABLE";
    case ConfigMutationOutcome::kOutcomeUnknown: return "OUTCOME_UNKNOWN";
  }
  return "UNAVAILABLE";
}

}  // namespace

void printUsbGeofenceMutationResult(const GeofenceMutationResult& result) {
  const char* state = "UNAVAILABLE";
  if (result.state == GeofenceResourceState::kClear) state = "CLEAR";
  else if (result.state == GeofenceResourceState::kConfigured) state = "CONFIGURED";
  // What the store holds now; after a refusal it is the previous resource.
  if (result.token_valid) {
    Serial.printf("APP FENCE id=%lu code=%s state=%s areas=%u vertices=%u "
                  "token=VALID revision=%lu\n",
                  static_cast<unsigned long>(result.request_id),
                  mutationCodeName(result.outcome), state,
                  static_cast<unsigned>(result.area_count),
                  static_cast<unsigned>(result.total_vertex_count),
                  static_cast<unsigned long>(result.token.revision));
  } else {
    Serial.printf("APP FENCE id=%lu code=%s state=%s areas=%u vertices=%u "
                  "token=UNAVAILABLE\n",
                  static_cast<unsigned long>(result.request_id),
                  mutationCodeName(result.outcome), state,
                  static_cast<unsigned>(result.area_count),
                  static_cast<unsigned>(result.total_vertex_count));
  }
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

    case ApplicationRequestKind::kGetPowerStatus: {
      const ApplicationPowerSnapshot& s = response.payload.power.snapshot;
      char percent[4] = {'-', '\0', '\0', '\0'};
      if (s.battery_valid && s.battery_percent <= 100)
        snprintf(percent, sizeof(percent), "%u",
                 static_cast<unsigned>(s.battery_percent));
      Serial.printf(
          "APP POWER id=%lu battery=%s state=%s percent_est=%s mv=%u raw=%u "
          "age_ms=%lu readings=%lu calibrated=%s\n",
          static_cast<unsigned long>(response.request_id),
          s.battery_valid ? "MEASURED" : "NONE",
          batteryStateName(s.battery_state), percent,
          static_cast<unsigned>(s.battery_mv),
          static_cast<unsigned>(s.battery_raw),
          static_cast<unsigned long>(s.battery_age_ms),
          static_cast<unsigned long>(s.battery_readings),
          s.battery_calibrated ? "yes" : "no");
      return;
    }

    case ApplicationRequestKind::kGetServicesStatus: {
      // Same line as before the typed query existed (scripts parse it).
      const ApplicationServicesSnapshot& s = response.payload.services.snapshot;
      char requested[5];
      formatServices(s.requested_services, requested, sizeof(requested));
      const bool explicit_mode =
          s.service_mode == config_format::kServiceModeExplicit;
      const char* applied = "LEGACY_AUTO";
      if (explicit_mode)
        applied = s.intent_applied ? "EXPLICIT" : "LEGACY_AUTO_UNSUPPORTED";
      Serial.printf(
          "APP SERVICES mode=%s requested=%s applied=%s tracking=%s relay=%s "
          "receive=%s legacy_mode=%s\n",
          explicit_mode ? "EXPLICIT" : "AUTO", explicit_mode ? requested : "-",
          applied, serviceStateName(s.tracking_state, s.tracking_reason),
          serviceStateName(s.relay_state, s.relay_reason),
          s.application_receive ? "ON" : "OFF",
          applicationRoleName(s.legacy_role));
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
