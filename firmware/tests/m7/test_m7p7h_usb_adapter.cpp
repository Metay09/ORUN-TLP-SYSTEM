// M7P7H: USB is an adapter over common typed application semantics.
// This test guards command spelling and confirms GET_CONFIG output remains
// byte-for-byte/text-compatible with the established M7P7D service command.
#include <assert.h>
#include <string.h>

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
  ApplicationResponse denied;
  denied.request_id = 18;
  denied.kind = ApplicationRequestKind::kGetStorageStatus;
  denied.code = ApplicationResponseCode::kAccessDenied;
  printUsbApplicationResponse(denied);
  assert(Serial.output == "APP RESULT id=18 code=ACCESS_DENIED\n");

  return 0;
}
