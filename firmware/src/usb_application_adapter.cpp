#include "usb_application_adapter.h"

#include <Arduino.h>

#include <string.h>

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
      Serial.printf(
          "APP DEVICE id=%lu rev=%u uptime_ms=%lu reset=0x%08lX watchdog=%s "
          "role=%u role_mode=%s gnss_presence=%u gnss_health=%u "
          "accel_presence=%u accel_health=%u tracking_state=%u "
          "tracking_reason=%u relay_state=%u relay_reason=%u\n",
          static_cast<unsigned long>(response.request_id),
          static_cast<unsigned>(s.surface_revision),
          static_cast<unsigned long>(s.uptime_ms),
          static_cast<unsigned long>(s.reset_reason),
          s.watchdog_reset ? "yes" : "no",
          static_cast<unsigned>(s.role),
          s.role_automatic ? "AUTO" : "OVERRIDE",
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
          "cadence=%u gnss_state=%u detected=%s extra_fix=%s "
          "attempts=%lu fresh=%lu timeouts=%lu invalid=%lu last_ttff_ms=%lu "
          "config_ready=%s source=%s\n",
          static_cast<unsigned long>(response.request_id),
          static_cast<unsigned long>(s.requested_interval_seconds),
          static_cast<unsigned long>(s.applied_base_interval_seconds),
          static_cast<unsigned long>(s.effective_interval_seconds),
          static_cast<unsigned>(s.cadence_mode),
          static_cast<unsigned>(s.gnss_state),
          s.gnss_detected ? "yes" : "no",
          s.additional_fix_active ? "yes" : "no",
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
          "overwritten=%lu append_failures=%lu corrupt=%lu metadata_failures=%lu "
          "config_ready=%s config_maintenance=%s config_corrupt=%lu "
          "geofence_ready=%s geofence_maintenance=%s geofence_corrupt=%lu "
          "security_ready=%s security_state=%u security_exhausted=%s "
          "security_corrupt=%lu\n",
          static_cast<unsigned long>(response.request_id),
          s.history_ready ? "yes" : "no",
          s.history_busy ? "yes" : "no",
          static_cast<unsigned long>(s.history_count),
          static_cast<unsigned long>(s.history_capacity),
          static_cast<unsigned long>(s.history_overwritten),
          static_cast<unsigned long>(s.history_append_failures),
          static_cast<unsigned long>(s.history_recovery_corruptions),
          static_cast<unsigned long>(s.history_metadata_failures),
          s.config_ready ? "yes" : "no",
          s.config_maintenance ? "yes" : "no",
          static_cast<unsigned long>(s.config_recovery_corruptions),
          s.geofence_ready ? "yes" : "no",
          s.geofence_maintenance ? "yes" : "no",
          static_cast<unsigned long>(s.geofence_recovery_corruptions),
          s.security_ready ? "yes" : "no",
          static_cast<unsigned>(s.security_state),
          s.security_exhausted ? "yes" : "no",
          static_cast<unsigned long>(s.security_recovery_corruptions));
      return;
    }
  }
}

}  // namespace orun_tlp
