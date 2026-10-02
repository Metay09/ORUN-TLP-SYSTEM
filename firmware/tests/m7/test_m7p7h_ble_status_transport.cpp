// M7P7H: additive BLE status families over the existing M7P7F framing.
// Validates that GET_CONFIG framing is untouched elsewhere while DEVICE,
// TRACKING, GEOFENCE and STORAGE remain <=48 bytes and use stop-and-wait
// fragmentation on the same characteristic pair.
#include <assert.h>
#include <string.h>

#include <array>

#include "application_request.h"
#include "application_status.h"
#include "ble_application_transport.h"
#include "config_store.h"
#include "storage_config.h"

using namespace orun_tlp;
namespace bat = orun_tlp::ble_app_transport;

namespace {

constexpr size_t kRegionSize =
    storage_config::kPageSize * storage_config::kFutureConfigRegionPages;

class ReadOnlyFlash : public FlashBackend {
 public:
  std::array<uint8_t, kRegionSize> bytes{};
  ReadOnlyFlash() { bytes.fill(0xFF); }
  bool begin() override { return true; }
  bool read(uint32_t offset, void* data, size_t size) const override {
    if (data == nullptr || offset > bytes.size() ||
        size > bytes.size() - offset)
      return false;
    memcpy(data, bytes.data() + offset, size);
    return true;
  }
  FlashOpResult program(uint32_t, const void*, size_t) override {
    return FlashOpResult::kFailed;
  }
  FlashOpResult erasePage(uint32_t) override {
    return FlashOpResult::kFailed;
  }
};

void writeLE16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}

uint16_t readLE16(const uint8_t* p) {
  return static_cast<uint16_t>(uint16_t(p[0]) | (uint16_t(p[1]) << 8));
}

uint32_t readLE32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
         (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

uint8_t buildQuery(uint8_t* out, bat::MessageType type, uint16_t correlation) {
  out[0] = bat::kTransportVersion;
  out[1] = static_cast<uint8_t>(type);
  out[2] = bat::kFlagStart | bat::kFlagEnd;
  out[3] = 0;
  writeLE16(out + 4, correlation);
  writeLE16(out + 6, 0);
  return bat::kHeaderSize;
}

uint16_t transact(BleApplicationTransport& transport, bat::MessageType request,
                  bat::MessageType response, uint16_t correlation,
                  uint8_t* logical) {
  uint8_t in[bat::kMaxFrameSize]{};
  const uint8_t in_len = buildQuery(in, request, correlation);
  const uint32_t generation = transport.currentSessionGeneration();
  transport.onFrameReceived(generation, in, in_len, 100);

  uint16_t copied = 0;
  uint8_t expected_fragment = 0;
  while (transport.outboundFramePending()) {
    uint8_t out[bat::kMaxFrameSize]{};
    uint8_t out_len = 0;
    assert(transport.peekOutboundFrame(generation, out, out_len));
    assert(out[0] == bat::kTransportVersion);
    assert(out[1] == static_cast<uint8_t>(response));
    assert(out[3] == expected_fragment);
    assert(readLE16(out + 4) == correlation);
    const uint16_t total = readLE16(out + 6);
    assert(total <= bat::kMaxLogicalPayload);
    const uint8_t chunk = static_cast<uint8_t>(out_len - bat::kHeaderSize);
    memcpy(logical + copied, out + bat::kHeaderSize, chunk);
    copied = static_cast<uint16_t>(copied + chunk);
    ++expected_fragment;
    transport.confirmOutboundFrame(generation);
  }
  return copied;
}

ApplicationStatusSnapshot makeSnapshot() {
  ApplicationStatusSnapshot s{};
  s.populated = 1;
  s.device.uptime_ms_mod32 = 0x11223344UL;
  s.device.reset_reason = 0xAABBCCDDUL;
  memcpy(s.device.firmware_version, "0.5.0-alpha", sizeof("0.5.0-alpha"));
  s.device.surface_revision = kApplicationSurfaceRevision;
  s.device.role = ApplicationRole::kTracker;
  s.device.role_automatic = 1;
  s.device.watchdog_reset = 1;
  s.device.relay_forwarding_applied = 0;
  s.device.gnss_presence = ApplicationPresence::kPresent;
  s.device.gnss_health = ApplicationHealth::kOk;
  s.device.accelerometer_presence = ApplicationPresence::kAbsent;
  s.device.accelerometer_health = ApplicationHealth::kUnavailable;
  s.device.tracking_state = ApplicationServiceState::kEnabled;
  s.device.tracking_reason = ApplicationServiceReason::kNone;
  // Resolved relay intent can be enabled while the radio has not yet
  // applied it. Byte 3 bit2 must reflect the latter, not the intent.
  s.device.relay_state = ApplicationServiceState::kEnabled;
  s.device.relay_reason = ApplicationServiceReason::kNone;

  s.tracking.requested_interval_seconds = 900;
  s.tracking.applied_base_interval_seconds = 600;
  s.tracking.effective_interval_seconds = 200;
  s.tracking.acquisition_attempts = 10;
  s.tracking.successful_fresh_fixes = 8;
  s.tracking.acquisition_timeouts = 1;
  s.tracking.invalid_fixes = 2;
  s.tracking.last_ttff_ms = 2760;
  s.tracking.config_backend_ready = 1;
  s.tracking.config_has_committed_record = 1;
  s.tracking.gnss_detected = 1;
  s.tracking.additional_fix_active = 1;
  s.tracking.cadence_mode = ApplicationCadenceMode::kBaseDividedBy3;
  s.tracking.gnss_state = ApplicationGnssState::kAcquiring;

  s.geofence.total_vertex_count = 64;
  s.geofence.area_count = 8;
  s.geofence.resource_state =
      ApplicationGeofenceResourceState::kConfigured;
  s.geofence.token_state = ApplicationGeofenceTokenState::kUncertain;
  s.geofence.runtime_configured = 1;
  s.geofence.has_confirmed_state = 1;
  s.geofence.confirmed_state =
      ApplicationGeofenceOperationalState::kOutside;
  s.geofence.confirmation_active = 1;
  s.geofence.cadence_mode = ApplicationCadenceMode::kBaseDividedBy3;

  s.storage.history_count = 625;
  s.storage.history_capacity = 728;
  s.storage.history_overwritten = 104;
  s.storage.history_append_failures = 2;
  s.storage.history_recovery_corruptions = 3;
  s.storage.history_metadata_failures = 4;
  s.storage.config_recovery_corruptions = 5;
  s.storage.geofence_recovery_corruptions = 6;
  s.storage.security_recovery_corruptions = 7;
  s.storage.history_ready = 1;
  s.storage.history_busy = 1;
  s.storage.config_ready = 1;
  s.storage.config_maintenance = 0;
  s.storage.geofence_ready = 1;
  s.storage.geofence_maintenance = 1;
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
  const ApplicationStatusSnapshot snapshot = makeSnapshot();
  ApplicationRequestService service(store, &snapshot);
  BleApplicationTransport transport(service);
  transport.beginSession();

  // Golden vectors freeze every logical byte before Android parsing/physical
  // qualification. DEVICE bit2 is actual relay-forwarding application state;
  // resolved relay intent remains separately encoded in bytes 10..11.
  {
    uint8_t payload[bat::kMaxLogicalPayload]{};
    const uint16_t len =
        transact(transport, bat::MessageType::kGetDeviceStatusRequest,
                 bat::MessageType::kGetDeviceStatusResponse, 0x1001, payload);
    const uint8_t expected[] = {
        0x00, 0x01, 0x00, 0x03, 0x01, 0x00, 0x02, 0x03,
        0x01, 0x00, 0x01, 0x00, 0x44, 0x33, 0x22, 0x11,
        0xDD, 0xCC, 0xBB, 0xAA, 0x30, 0x2E, 0x35, 0x2E,
        0x30, 0x2D, 0x61, 0x6C, 0x70, 0x68, 0x61, 0x00,
        0x00, 0x00, 0x00, 0x00};
    assert(len == sizeof(expected));
    assert(memcmp(payload, expected, sizeof(expected)) == 0);
  }

  // TRACKING: full 36-byte golden, including cadence/state and all diagnostics.
  {
    uint8_t payload[bat::kMaxLogicalPayload]{};
    const uint16_t len =
        transact(transport, bat::MessageType::kGetTrackingStatusRequest,
                 bat::MessageType::kGetTrackingStatusResponse, 0x1002, payload);
    const uint8_t expected[] = {
        0x00, 0x0F, 0x01, 0x06,
        0x84, 0x03, 0x00, 0x00,
        0x58, 0x02, 0x00, 0x00,
        0xC8, 0x00, 0x00, 0x00,
        0x0A, 0x00, 0x00, 0x00,
        0x08, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x00, 0x00,
        0xC8, 0x0A, 0x00, 0x00};
    assert(len == sizeof(expected));
    assert(memcmp(payload, expected, sizeof(expected)) == 0);
  }

  // GEOFENCE: full summary golden; no geometry/token bytes.
  {
    uint8_t payload[bat::kMaxLogicalPayload]{};
    const uint16_t len =
        transact(transport, bat::MessageType::kGetGeofenceStatusRequest,
                 bat::MessageType::kGetGeofenceStatusResponse, 0x1003, payload);
    const uint8_t expected[] = {
        0x00, 0x02, 0x02, 0x07, 0x08, 0x40, 0x00, 0x02, 0x01};
    assert(len == sizeof(expected));
    assert(memcmp(payload, expected, sizeof(expected)) == 0);
  }

  // STORAGE: full 40-byte golden, freezing every counter/flag position.
  {
    uint8_t payload[bat::kMaxLogicalPayload]{};
    const uint16_t len =
        transact(transport, bat::MessageType::kGetStorageStatusRequest,
                 bat::MessageType::kGetStorageStatusResponse, 0x1004, payload);
    const uint8_t expected[] = {
        0x00, 0x77, 0x01, 0x00,
        0x71, 0x02, 0x00, 0x00,
        0xD8, 0x02, 0x00, 0x00,
        0x68, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x00, 0x00,
        0x03, 0x00, 0x00, 0x00,
        0x04, 0x00, 0x00, 0x00,
        0x05, 0x00, 0x00, 0x00,
        0x06, 0x00, 0x00, 0x00,
        0x07, 0x00, 0x00, 0x00};
    assert(len == sizeof(expected));
    assert(memcmp(payload, expected, sizeof(expected)) == 0);
  }

  // Every M7P7H status query is exactly zero-payload. A syntactically complete
  // one-byte request is malformed and must fail closed: no response and no
  // ApplicationRequestService slot acquisition.
  for (bat::MessageType type : {
           bat::MessageType::kGetDeviceStatusRequest,
           bat::MessageType::kGetTrackingStatusRequest,
           bat::MessageType::kGetGeofenceStatusRequest,
           bat::MessageType::kGetStorageStatusRequest}) {
    uint8_t frame[bat::kMaxFrameSize]{};
    frame[0] = bat::kTransportVersion;
    frame[1] = static_cast<uint8_t>(type);
    frame[2] = bat::kFlagStart | bat::kFlagEnd;
    frame[3] = 0;
    writeLE16(frame + 4, 0x2200);
    writeLE16(frame + 6, 1);
    frame[8] = 0xA5;
    transport.onFrameReceived(transport.currentSessionGeneration(), frame, 9,
                              200);
    assert(!transport.outboundFramePending());
    assert(!service.responsePending());
  }

  // A known status kind with no populated snapshot maps to the additive
  // ERROR/UNAVAILABLE wire result: [0x04, offending-type].
  {
    ApplicationRequestService no_status_service(store);
    BleApplicationTransport no_status_transport(no_status_service);
    no_status_transport.beginSession();
    uint8_t payload[bat::kMaxLogicalPayload]{};
    const uint16_t len =
        transact(no_status_transport, bat::MessageType::kGetDeviceStatusRequest,
                 bat::MessageType::kError, 0x3301, payload);
    const uint8_t expected[] = {
        static_cast<uint8_t>(bat::ErrorCode::kUnavailable),
        static_cast<uint8_t>(bat::MessageType::kGetDeviceStatusRequest)};
    assert(len == sizeof(expected));
    assert(memcmp(payload, expected, sizeof(expected)) == 0);
  }

  return 0;
}
