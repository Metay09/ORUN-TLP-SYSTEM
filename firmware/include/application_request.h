#pragma once

#include <stdint.h>

#include "application_status.h"

namespace orun_tlp {

class ConfigStore;

// M7P7D/M7P7E established the typed, transport-neutral request seam.
// M7P7H keeps the same fixed-memory, loop-owned model but expands the
// read-only surface without turning one response struct into a monolith.
//
// This is deliberately NOT a wire format. USB/BLE/future LoRa adapters
// translate transport input into typed requests and typed results.
//
// ApplicationRequester is LOCAL adapter provenance, not user identity,
// authorization, connection identity or a future wire field.
enum class ApplicationRequester : uint8_t {
  kUsb = 1,
  kBle = 2,
};

enum class ApplicationRequestKind : uint8_t {
  kGetConfig = 1,
  kGetDeviceStatus = 2,
  kGetTrackingStatus = 3,
  kGetGeofenceStatus = 4,
  kGetStorageStatus = 5,
};

enum class ApplicationResponseCode : uint8_t {
  kOk = 0,
  kUnsupported = 1,
  kUnavailable = 2,
  kAccessDenied = 3,
};

enum class ApplicationSubmitResult : uint8_t {
  kAccepted = 0,
  kBusy = 1,
  kRejected = 2,
};

// Access/security facts are intentionally separate from requester provenance.
// This is not final user authorization; it is the central seam that prevents
// each transport adapter from growing its own product-operation allow-list.
enum class ApplicationAccessChannel : uint8_t {
  kInvalid = 0,
  kUsbLocal = 1,
  kBleOpen = 2,
  kBleEncrypted = 3,
};

struct ApplicationAccessContext {
  constexpr ApplicationAccessContext(
      ApplicationAccessChannel channel_value = ApplicationAccessChannel::kInvalid)
      : channel(channel_value) {}

  ApplicationAccessChannel channel;
};

constexpr ApplicationAccessContext defaultApplicationAccess(
    ApplicationRequester requester) {
  return requester == ApplicationRequester::kUsb
             ? ApplicationAccessContext(ApplicationAccessChannel::kUsbLocal)
             : requester == ApplicationRequester::kBle
                   ? ApplicationAccessContext(ApplicationAccessChannel::kBleOpen)
                   : ApplicationAccessContext(ApplicationAccessChannel::kInvalid);
}

struct ApplicationRequest {
  constexpr ApplicationRequest(
      ApplicationRequester requester_value,
      uint32_t request_id_value = 0,
      ApplicationRequestKind kind_value = ApplicationRequestKind::kGetConfig)
      : requester(requester_value),
        request_id(request_id_value),
        kind(kind_value),
        access(defaultApplicationAccess(requester_value)) {}

  constexpr ApplicationRequest(
      ApplicationRequester requester_value,
      uint32_t request_id_value,
      ApplicationRequestKind kind_value,
      ApplicationAccessContext access_value)
      : requester(requester_value),
        request_id(request_id_value),
        kind(kind_value),
        access(access_value) {}

  ApplicationRequester requester;
  uint32_t request_id;
  ApplicationRequestKind kind;
  ApplicationAccessContext access;
};

// Response payloads are constructor-free POD so the union remains compatible
// with the repository's gnu++11 production toolchain.
struct ApplicationConfigResponsePayload {
  uint32_t tracking_interval_seconds;
  uint32_t battery_capacity_mah;
  uint8_t backend_ready;
  uint8_t has_committed_record;
};

struct ApplicationDeviceResponsePayload {
  ApplicationDeviceSnapshot snapshot;
};

struct ApplicationTrackingResponsePayload {
  ApplicationTrackingSnapshot snapshot;
};

struct ApplicationGeofenceResponsePayload {
  ApplicationGeofenceSnapshot snapshot;
};

struct ApplicationStorageResponsePayload {
  ApplicationStorageSnapshot snapshot;
};

union ApplicationResponsePayload {
  ApplicationConfigResponsePayload config;
  ApplicationDeviceResponsePayload device;
  ApplicationTrackingResponsePayload tracking;
  ApplicationGeofenceResponsePayload geofence;
  ApplicationStorageResponsePayload storage;
  uint8_t raw[48];
};

struct ApplicationResponse {
  ApplicationResponse()
      : requester(ApplicationRequester::kUsb),
        request_id(0),
        kind(ApplicationRequestKind::kGetConfig),
        code(ApplicationResponseCode::kUnsupported),
        payload{} {}

  ApplicationRequester requester;
  uint32_t request_id;
  ApplicationRequestKind kind;
  ApplicationResponseCode code;
  ApplicationResponsePayload payload;
};

static_assert(sizeof(ApplicationResponsePayload) <= 48,
              "application response payload must remain bounded");

// One response slot remains intentional read-path backpressure. It is NOT a
// future durable mutation lock; side-effecting operations require domain-owned
// serialized mutation lifecycles.
class ApplicationRequestService {
 public:
  explicit ApplicationRequestService(
      ConfigStore& config_store,
      const ApplicationStatusSnapshot* status_snapshot = nullptr)
      : config_store_(config_store),
        status_snapshot_(status_snapshot),
        response_ready_(false),
        response_() {}

  ApplicationSubmitResult submit(const ApplicationRequest& request);
  bool takeResponse(ApplicationRequester requester, ApplicationResponse& response);
  bool discardResponse(ApplicationRequester requester);
  bool responsePending() const { return response_ready_; }

 private:
  static bool requesterSupported(ApplicationRequester requester);
  static bool accessMatchesRequester(
      ApplicationRequester requester, const ApplicationAccessContext& access);
  static bool requestKindSupported(ApplicationRequestKind kind);
  static bool accessAllowed(ApplicationRequestKind kind,
                            const ApplicationAccessContext& access);

  ConfigStore& config_store_;
  const ApplicationStatusSnapshot* status_snapshot_;
  bool response_ready_;
  ApplicationResponse response_;
};

}  // namespace orun_tlp
