// M7P7H: USB is an adapter over common typed application semantics.
// This test guards command spelling and confirms GET_CONFIG output remains
// byte-for-byte/text-compatible with the established M7P7D service command.
#include <assert.h>
#include <string.h>
#include <string>

#include "Arduino.h"
#include "application_request.h"
#include "usb_application_adapter.h"

using namespace orun_tlp;

namespace {

ApplicationRequestKind parse(const char* text) {
  ApplicationRequestKind kind = ApplicationRequestKind::kGetConfig;
  assert(parseUsbApplicationQuery(
      text, static_cast<uint8_t>(strlen(text)), &kind));
  return kind;
}

}  // namespace

int main() {
  assert(parse("APP CONFIG?") == ApplicationRequestKind::kGetConfig);
  assert(parse("APP DEVICE?") == ApplicationRequestKind::kGetDeviceStatus);
  assert(parse("APP TRACKING?") == ApplicationRequestKind::kGetTrackingStatus);
  assert(parse("APP GEOFENCE?") == ApplicationRequestKind::kGetGeofenceStatus);
  assert(parse("APP STORAGE?") == ApplicationRequestKind::kGetStorageStatus);

  ApplicationRequestKind untouched = ApplicationRequestKind::kGetDeviceStatus;
  assert(!parseUsbApplicationQuery("ROLE?", 5, &untouched));
  assert(untouched == ApplicationRequestKind::kGetDeviceStatus);
  assert(!parseUsbApplicationQuery("APP DEVICE?x", 12, &untouched));
  assert(!parseUsbApplicationQuery(nullptr, 0, &untouched));
  assert(!parseUsbApplicationQuery("APP DEVICE?", 11, nullptr));

  Serial.output.clear();
  ApplicationResponse config;
  config.requester = ApplicationRequester::kUsb;
  config.request_id = 17;
  config.kind = ApplicationRequestKind::kGetConfig;
  config.code = ApplicationResponseCode::kOk;
  config.payload.config.backend_ready = 1;
  config.payload.config.has_committed_record = 0;
  config.payload.config.tracking_interval_seconds = 180;
  config.payload.config.battery_capacity_mah = 0;
  printUsbApplicationResponse(config);
  assert(Serial.output ==
         "APP RESULT id=17 code=OK config_backend_ready=yes source=default "
         "tracking_interval_seconds=180 battery_capacity_mah=0\n");

  Serial.output.clear();
  ApplicationResponse device;
  device.requester = ApplicationRequester::kUsb;
  device.request_id = 18;
  device.kind = ApplicationRequestKind::kGetDeviceStatus;
  device.code = ApplicationResponseCode::kOk;
  device.payload.device.snapshot.surface_revision = kApplicationSurfaceRevision;
  memcpy(device.payload.device.snapshot.firmware_version, "0.5.0-alpha",
         sizeof("0.5.0-alpha"));
  device.payload.device.snapshot.uptime_ms_mod32 = 0xFFFFFFFFUL;
  device.payload.device.snapshot.role = ApplicationRole::kRelay;
  device.payload.device.snapshot.role_automatic = 0;
  device.payload.device.snapshot.watchdog_reset = 0;
  device.payload.device.snapshot.relay_forwarding_applied = 0;
  device.payload.device.snapshot.relay_state = ApplicationServiceState::kEnabled;
  device.payload.device.snapshot.relay_reason = ApplicationServiceReason::kNone;
  printUsbApplicationResponse(device);
  assert(Serial.output.find("uptime_ms_mod32=4294967295") !=
         std::string::npos);
  assert(Serial.output.find("relay_applied=no") != std::string::npos);

  Serial.output.clear();
  ApplicationResponse denied;
  denied.request_id = 19;
  denied.kind = ApplicationRequestKind::kGetStorageStatus;
  denied.code = ApplicationResponseCode::kAccessDenied;
  printUsbApplicationResponse(denied);
  assert(Serial.output == "APP RESULT id=19 code=ACCESS_DENIED\n");

  // Status lines are longer than the target's 256-byte Print::printf buffer.
  // On a RAK4631 the single-printf form cut APP DEVICE at 255 characters and
  // ended APP STORAGE in stack bytes. The test Serial enforces the per-call
  // limit; these cases prove each complete line still arrives byte-for-byte.
  {
    // The exact line a tracker printed before the fix, minus the cut.
    Serial.output.clear();
    ApplicationResponse observed;
    observed.request_id = 1;
    observed.kind = ApplicationRequestKind::kGetDeviceStatus;
    observed.code = ApplicationResponseCode::kOk;
    ApplicationDeviceSnapshot& d = observed.payload.device.snapshot;
    d.surface_revision = 1;
    memcpy(d.firmware_version, "0.5.0-alpha", sizeof("0.5.0-alpha"));
    d.uptime_ms_mod32 = 1150237;
    d.reset_reason = 2;
    d.watchdog_reset = 1;
    d.role = static_cast<ApplicationRole>(0);
    d.role_automatic = 1;
    d.gnss_presence = static_cast<ApplicationPresence>(1);
    d.accelerometer_presence = static_cast<ApplicationPresence>(1);
    d.tracking_state = static_cast<ApplicationServiceState>(1);
    d.relay_reason = static_cast<ApplicationServiceReason>(1);
    printUsbApplicationResponse(observed);
    assert(Serial.output ==
           "APP DEVICE id=1 fw=0.5.0-alpha rev=1 uptime_ms_mod32=1150237 "
           "reset=0x00000002 watchdog=yes role=0 role_mode=AUTO "
           "relay_applied=no gnss_presence=1 gnss_health=0 accel_presence=1 "
           "accel_health=0 tracking_state=1 tracking_reason=0 relay_state=0 "
           "relay_reason=1\n");
    assert(Serial.output.size() > 255);

    // Widest possible values of every numeric field.
    Serial.output.clear();
    ApplicationResponse storage;
    storage.request_id = UINT32_MAX;
    storage.kind = ApplicationRequestKind::kGetStorageStatus;
    storage.code = ApplicationResponseCode::kOk;
    ApplicationStorageSnapshot& s = storage.payload.storage.snapshot;
    s.history_count = s.history_capacity = s.history_overwritten = UINT32_MAX;
    s.history_append_failures = s.history_recovery_corruptions = UINT32_MAX;
    s.history_metadata_failures = s.config_recovery_corruptions = UINT32_MAX;
    s.geofence_recovery_corruptions = UINT32_MAX;
    s.security_recovery_corruptions = UINT32_MAX;
    s.history_ready = s.history_busy = s.config_ready = 1;
    s.config_maintenance = s.geofence_ready = s.geofence_maintenance = 1;
    s.security_ready = s.security_exhausted = 1;
    s.security_state = static_cast<ApplicationSecurityState>(255);
    printUsbApplicationResponse(storage);
    assert(Serial.output ==
           "APP STORAGE id=4294967295 history_ready=yes busy=yes "
           "count=4294967295 capacity=4294967295 overwritten=4294967295 "
           "append_failures=4294967295 corrupt=4294967295 "
           "metadata_failures=4294967295 config_ready=yes "
           "config_maintenance=yes config_corrupt=4294967295 "
           "geofence_ready=yes geofence_maintenance=yes "
           "geofence_corrupt=4294967295 security_ready=yes security_state=255 "
           "security_exhausted=yes security_corrupt=4294967295\n");

    Serial.output.clear();
    ApplicationResponse tracking;
    tracking.request_id = UINT32_MAX;
    tracking.kind = ApplicationRequestKind::kGetTrackingStatus;
    tracking.code = ApplicationResponseCode::kOk;
    ApplicationTrackingSnapshot& t = tracking.payload.tracking.snapshot;
    t.requested_interval_seconds = t.applied_base_interval_seconds = UINT32_MAX;
    t.effective_interval_seconds = t.acquisition_attempts = UINT32_MAX;
    t.successful_fresh_fixes = t.acquisition_timeouts = UINT32_MAX;
    t.invalid_fixes = t.last_ttff_ms = UINT32_MAX;
    t.config_backend_ready = t.gnss_detected = t.additional_fix_active = 1;
    t.cadence_mode = static_cast<ApplicationCadenceMode>(255);
    t.gnss_state = static_cast<ApplicationGnssState>(255);
    printUsbApplicationResponse(tracking);
    assert(Serial.output ==
           "APP TRACKING id=4294967295 requested_s=4294967295 "
           "applied_s=4294967295 effective_s=4294967295 cadence=255 "
           "gnss_state=255 detected=yes extra_fix=yes attempts=4294967295 "
           "fresh=4294967295 timeouts=4294967295 invalid=4294967295 "
           "last_ttff_ms=4294967295 config_ready=yes source=default\n");
  }

  // Configuration change spelling: APP INTERVAL <seconds>, strict decimal.
  {
    ConfigMutationKind kind = ConfigMutationKind::kSetTrackingInterval;
    uint32_t value = 0;
    const auto parse_config = [&](const char* text) {
      return parseUsbConfigCommand(
          text, static_cast<uint8_t>(strlen(text)), &kind, &value);
    };
    assert(parse_config("APP INTERVAL 900") == UsbConfigCommandParse::kOk);
    assert(kind == ConfigMutationKind::kSetTrackingInterval && value == 900);
    // The widest value still fits the 23-character USB command buffer.
    assert(strlen("APP INTERVAL 4294967295") == 23);
    assert(parse_config("APP INTERVAL 4294967295") ==
           UsbConfigCommandParse::kOk);
    assert(value == UINT32_MAX);
    assert(parse_config("APP INTERVAL 0") == UsbConfigCommandParse::kOk);
    assert(value == 0);  // range is the owner's decision, not the parser's

    // Recognized command, unusable argument: never silently reinterpreted.
    value = 77;
    const char* malformed[] = {
        "APP INTERVAL ",      "APP INTERVAL 9x",    "APP INTERVAL x9",
        "APP INTERVAL -5",    "APP INTERVAL +5",    "APP INTERVAL  5",
        "APP INTERVAL 5 ",    "APP INTERVAL 0x10",  "APP INTERVAL 4294967296",
        "APP INTERVAL 99999999999"};
    for (const char* text : malformed) {
      assert(parse_config(text) == UsbConfigCommandParse::kMalformed);
      assert(value == 77);
    }

    // Other commands are left to their own parsers.
    const char* unrelated[] = {"APP INTERVAL", "APP INTERVAL?", "APP CONFIG?",
                               "app interval 900", "ROLE?", ""};
    for (const char* text : unrelated) {
      assert(parse_config(text) == UsbConfigCommandParse::kNotConfigCommand);
      assert(value == 77);
    }
    assert(parseUsbConfigCommand(nullptr, 0, &kind, &value) ==
           UsbConfigCommandParse::kNotConfigCommand);
    assert(parseUsbConfigCommand("APP INTERVAL 900", 16, nullptr, &value) ==
           UsbConfigCommandParse::kNotConfigCommand);
    assert(parseUsbConfigCommand("APP INTERVAL 900", 16, &kind, nullptr) ==
           UsbConfigCommandParse::kNotConfigCommand);

    ConfigMutationResult applied;
    applied.request_id = 40;
    applied.outcome = ConfigMutationOutcome::kApplied;
    applied.token_valid = true;
    applied.token = config_format::StateToken(0x1122334455667788ULL, 2);
    applied.config = config_format::Config(600, 0);
    Serial.output.clear();
    printUsbConfigMutationResult(applied);
    assert(Serial.output ==
           "APP SET id=40 code=APPLIED tracking_interval_seconds=600 "
           "token=VALID revision=2\n");

    // Without a VALID token nothing token-like is printed, and the interval
    // shown is the store's value, not the requested one.
    ConfigMutationResult unknown;
    unknown.request_id = 41;
    unknown.outcome = ConfigMutationOutcome::kOutcomeUnknown;
    unknown.config = config_format::Config(180, 0);
    Serial.output.clear();
    printUsbConfigMutationResult(unknown);
    assert(Serial.output ==
           "APP SET id=41 code=OUTCOME_UNKNOWN tracking_interval_seconds=180 "
           "token=UNAVAILABLE\n");

    const struct {
      ConfigMutationOutcome outcome;
      const char* text;
    } names[] = {
        {ConfigMutationOutcome::kUnchanged, "code=UNCHANGED "},
        {ConfigMutationOutcome::kInvalid, "code=INVALID "},
        {ConfigMutationOutcome::kAccessDenied, "code=ACCESS_DENIED "},
        {ConfigMutationOutcome::kMaintenance, "code=MAINTENANCE "},
        {ConfigMutationOutcome::kUnavailable, "code=UNAVAILABLE "},
    };
    for (const auto& name : names) {
      ConfigMutationResult result;
      result.outcome = name.outcome;
      Serial.output.clear();
      printUsbConfigMutationResult(result);
      assert(Serial.output.find(name.text) != std::string::npos);
    }
  }

  return 0;
}
