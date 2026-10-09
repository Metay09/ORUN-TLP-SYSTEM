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
