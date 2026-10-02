// M7P7H: transport-neutral bounded status families and central access seam.
// Uses a synthetic POD status snapshot so ApplicationRequestService remains
// independent from Arduino/GNSS/radio driver headers.
#include <assert.h>
#include <string.h>

#include <array>

#include "application_request.h"
#include "application_status.h"
#include "config_store.h"
#include "storage_config.h"

using namespace orun_tlp;

namespace {

constexpr size_t kRegionSize =
    storage_config::kPageSize * storage_config::kFutureConfigRegionPages;

class ReadOnlyFlash : public FlashBackend {
 public:
  std::array<uint8_t, kRegionSize> bytes{};
  unsigned program_calls = 0;
  unsigned erase_calls = 0;

  ReadOnlyFlash() { bytes.fill(0xFF); }

  bool begin() override { return true; }

  bool read(uint32_t offset, void* data, size_t size) const override {
    if (data == nullptr || offset > bytes.size() ||
        size > bytes.size() - offset) {
      return false;
    }
    memcpy(data, bytes.data() + offset, size);
    return true;
  }

  FlashOpResult program(uint32_t, const void*, size_t) override {
    ++program_calls;
    return FlashOpResult::kFailed;
  }

  FlashOpResult erasePage(uint32_t) override {
    ++erase_calls;
    return FlashOpResult::kFailed;
  }
};

ApplicationResponse submitTake(ApplicationRequestService& service,
                               ApplicationRequester requester,
                               uint32_t id,
                               ApplicationRequestKind kind,
                               ApplicationAccessContext access) {
  assert(service.submit(ApplicationRequest(requester, id, kind, access)) ==
         ApplicationSubmitResult::kAccepted);
  ApplicationResponse response;
  assert(service.takeResponse(requester, response));
  assert(response.requester == requester);
  assert(response.request_id == id);
  assert(response.kind == kind);
  return response;
}

ApplicationStatusSnapshot sampleSnapshot() {
  ApplicationStatusSnapshot s{};
  s.populated = 1;

  s.device.uptime_ms_mod32 = 123456;
  s.device.reset_reason = 0xA5A50011UL;
  memcpy(s.device.firmware_version, "0.5.0-alpha", sizeof("0.5.0-alpha"));
  s.device.surface_revision = kApplicationSurfaceRevision;
  s.device.role = ApplicationRole::kTracker;
  s.device.role_automatic = 1;
  s.device.watchdog_reset = 0;
  s.device.relay_forwarding_applied = 0;
  s.device.gnss_presence = ApplicationPresence::kPresent;
  s.device.gnss_health = ApplicationHealth::kOk;
  s.device.accelerometer_presence = ApplicationPresence::kPresent;
  s.device.accelerometer_health = ApplicationHealth::kDegraded;
  s.device.tracking_state = ApplicationServiceState::kEnabled;
  s.device.tracking_reason = ApplicationServiceReason::kNone;
  s.device.relay_state = ApplicationServiceState::kEnabled;
  s.device.relay_reason = ApplicationServiceReason::kNone;

  s.tracking.requested_interval_seconds = 600;
  s.tracking.applied_base_interval_seconds = 180;
  s.tracking.effective_interval_seconds = 60;
  s.tracking.acquisition_attempts = 44;
  s.tracking.successful_fresh_fixes = 40;
  s.tracking.acquisition_timeouts = 2;
  s.tracking.invalid_fixes = 3;
  s.tracking.last_ttff_ms = 2870;
  s.tracking.config_backend_ready = 1;
  s.tracking.config_has_committed_record = 1;
  s.tracking.gnss_detected = 1;
  s.tracking.additional_fix_active = 0;
  s.tracking.cadence_mode = ApplicationCadenceMode::kBaseDividedBy3;
  s.tracking.gnss_state = ApplicationGnssState::kSleeping;

  s.geofence.total_vertex_count = 7;
  s.geofence.area_count = 2;
  s.geofence.resource_state =
      ApplicationGeofenceResourceState::kConfigured;
  s.geofence.token_state = ApplicationGeofenceTokenState::kValid;
  s.geofence.runtime_configured = 1;
  s.geofence.has_confirmed_state = 1;
  s.geofence.confirmed_state =
      ApplicationGeofenceOperationalState::kOutside;
  s.geofence.confirmation_active = 0;
  s.geofence.cadence_mode = ApplicationCadenceMode::kBaseDividedBy3;

  s.storage.history_count = 625;
  s.storage.history_capacity = 728;
  s.storage.history_overwritten = 104;
  s.storage.history_append_failures = 1;
  s.storage.history_recovery_corruptions = 2;
  s.storage.history_metadata_failures = 3;
  s.storage.config_recovery_corruptions = 4;
  s.storage.geofence_recovery_corruptions = 5;
  s.storage.security_recovery_corruptions = 6;
  s.storage.history_ready = 1;
  s.storage.history_busy = 0;
  s.storage.config_ready = 1;
  s.storage.config_maintenance = 0;
  s.storage.geofence_ready = 1;
  s.storage.geofence_maintenance = 0;
  s.storage.security_ready = 1;
  s.storage.security_exhausted = 0;
  s.storage.security_state = ApplicationSecurityState::kProvisioned;

  return s;
}

}  // namespace

int main() {
  ReadOnlyFlash flash;
  ConfigStore store(flash);
  assert(store.begin());

  const ApplicationStatusSnapshot snapshot = sampleSnapshot();
  ApplicationRequestService service(store, &snapshot);

  // 1. USB and BLE both read the exact same typed DEVICE snapshot.
  {
    const ApplicationResponse usb = submitTake(
        service, ApplicationRequester::kUsb, 1,
        ApplicationRequestKind::kGetDeviceStatus,
        ApplicationAccessContext(ApplicationAccessChannel::kUsbLocal));
    assert(usb.code == ApplicationResponseCode::kOk);
    assert(usb.payload.device.snapshot.uptime_ms_mod32 == 123456);
    assert(usb.payload.device.snapshot.role == ApplicationRole::kTracker);
    assert(usb.payload.device.snapshot.relay_state ==
           ApplicationServiceState::kEnabled);
    assert(usb.payload.device.snapshot.relay_forwarding_applied == 0);
    assert(strcmp(usb.payload.device.snapshot.firmware_version,
                  "0.5.0-alpha") == 0);

    const ApplicationResponse ble = submitTake(
        service, ApplicationRequester::kBle, 2,
        ApplicationRequestKind::kGetDeviceStatus,
        ApplicationAccessContext(ApplicationAccessChannel::kBleOpen));
    assert(ble.code == ApplicationResponseCode::kOk);
    assert(memcmp(&usb.payload.device.snapshot,
                  &ble.payload.device.snapshot,
                  sizeof(ApplicationDeviceSnapshot)) == 0);
  }

  // 2. Remaining families are bounded copies from the same snapshot owner.
  {
    const ApplicationResponse tracking = submitTake(
        service, ApplicationRequester::kUsb, 3,
        ApplicationRequestKind::kGetTrackingStatus,
        ApplicationAccessContext(ApplicationAccessChannel::kUsbLocal));
    assert(tracking.code == ApplicationResponseCode::kOk);
    assert(tracking.payload.tracking.snapshot.requested_interval_seconds == 600);
    assert(tracking.payload.tracking.snapshot.applied_base_interval_seconds ==
           180);
    assert(tracking.payload.tracking.snapshot.effective_interval_seconds == 60);
    assert(tracking.payload.tracking.snapshot.last_ttff_ms == 2870);

    const ApplicationResponse geofence = submitTake(
        service, ApplicationRequester::kBle, 4,
        ApplicationRequestKind::kGetGeofenceStatus,
        ApplicationAccessContext(ApplicationAccessChannel::kBleEncrypted));
    assert(geofence.code == ApplicationResponseCode::kOk);
    assert(geofence.payload.geofence.snapshot.area_count == 2);
    assert(geofence.payload.geofence.snapshot.total_vertex_count == 7);
    assert(geofence.payload.geofence.snapshot.confirmed_state ==
           ApplicationGeofenceOperationalState::kOutside);

    const ApplicationResponse storage = submitTake(
        service, ApplicationRequester::kUsb, 5,
        ApplicationRequestKind::kGetStorageStatus,
        ApplicationAccessContext(ApplicationAccessChannel::kUsbLocal));
    assert(storage.code == ApplicationResponseCode::kOk);
    assert(storage.payload.storage.snapshot.history_count == 625);
    assert(storage.payload.storage.snapshot.history_capacity == 728);
    assert(storage.payload.storage.snapshot.security_state ==
           ApplicationSecurityState::kProvisioned);
  }

  // 3. The access decision is central and distinct from requester provenance.
  // A known operation with an invalid context produces ACCESS_DENIED rather
  // than being silently allowed by an adapter-local allow-list.
  {
    const ApplicationResponse denied = submitTake(
        service, ApplicationRequester::kBle, 6,
        ApplicationRequestKind::kGetStorageStatus,
        ApplicationAccessContext(ApplicationAccessChannel::kInvalid));
    assert(denied.code == ApplicationResponseCode::kAccessDenied);
  }

  // Requester provenance and access facts cannot be cross-wired. This is an
  // internal adapter invariant and fails before a response slot is acquired.
  {
    assert(service.submit(ApplicationRequest(
               ApplicationRequester::kUsb, 61,
               ApplicationRequestKind::kGetDeviceStatus,
               ApplicationAccessContext(ApplicationAccessChannel::kBleOpen))) ==
           ApplicationSubmitResult::kRejected);
    assert(!service.responsePending());
    assert(service.submit(ApplicationRequest(
               ApplicationRequester::kBle, 62,
               ApplicationRequestKind::kGetDeviceStatus,
               ApplicationAccessContext(ApplicationAccessChannel::kUsbLocal))) ==
           ApplicationSubmitResult::kRejected);
    assert(!service.responsePending());
  }

  // 4. Unsupported kind remains UNSUPPORTED even when access is invalid.
  {
    const ApplicationResponse unsupported = submitTake(
        service, ApplicationRequester::kUsb, 7,
        static_cast<ApplicationRequestKind>(0xFE),
        ApplicationAccessContext(ApplicationAccessChannel::kInvalid));
    assert(unsupported.code == ApplicationResponseCode::kUnsupported);
  }

  // 5. A present-but-never-populated snapshot is also UNAVAILABLE. This
  // prevents a future adapter (including LoRa) from accidentally returning the
  // boot-time zero image as authoritative status if it forgets to refresh.
  {
    ApplicationStatusSnapshot empty{};
    ApplicationRequestService unpopulated(store, &empty);
    const ApplicationResponse unavailable = submitTake(
        unpopulated, ApplicationRequester::kUsb, 71,
        ApplicationRequestKind::kGetDeviceStatus,
        ApplicationAccessContext(ApplicationAccessChannel::kUsbLocal));
    assert(unavailable.code == ApplicationResponseCode::kUnavailable);
  }

  // 6. A service without a status snapshot keeps GET_CONFIG usable while new
  // status families fail closed as UNAVAILABLE.
  ApplicationRequestService no_status(store);
  {
    const ApplicationResponse unavailable = submitTake(
        no_status, ApplicationRequester::kUsb, 8,
        ApplicationRequestKind::kGetDeviceStatus,
        ApplicationAccessContext(ApplicationAccessChannel::kUsbLocal));
    assert(unavailable.code == ApplicationResponseCode::kUnavailable);

    const ApplicationResponse config = submitTake(
        no_status, ApplicationRequester::kUsb, 9,
        ApplicationRequestKind::kGetConfig,
        ApplicationAccessContext(ApplicationAccessChannel::kUsbLocal));
    assert(config.code == ApplicationResponseCode::kOk);
  }

  assert(flash.program_calls == 0);
  assert(flash.erase_calls == 0);
  return 0;
}
