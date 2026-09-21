#pragma once

#include <stdint.h>

#include "config_format.h"

namespace orun_tlp {

class ConfigStore;

// M7P7D/M7P7E: typed, transport-neutral application request seam.
//
// This is deliberately NOT a wire format. USB/BLE/other adapters translate
// their bounded transport input into one of these typed requests. The request
// service remains loop-task owned; BLE callbacks must never call it directly.
//
// M7P7E adds explicit requester ownership before a second transport adapter is
// allowed to exist. A response may be consumed only by the requester that
// submitted the accepted request; another adapter cannot steal or clear it.
//
// ApplicationRequester is LOCAL adapter provenance, not user identity,
// authorization, connection identity or a future wire field. Each adapter must
// assign its own constant requester value; peer-controlled bytes must never be
// allowed to choose kUsb/kBle.
//
// The seam still exposes only one safe, read-only operation. Protected config
// mutation, provisioning, MESSAGE and command/control requests remain later
// work behind their reviewed authorization/security gates.
enum class ApplicationRequester : uint8_t {
  kUsb = 1,
  kBle = 2,
};

enum class ApplicationRequestKind : uint8_t {
  kGetConfig = 1,
};

enum class ApplicationResponseCode : uint8_t {
  kOk = 0,
  kUnsupported = 1,
};

enum class ApplicationSubmitResult : uint8_t {
  kAccepted = 0,
  kBusy = 1,
  kRejected = 2,
};

struct ApplicationRequest {
  constexpr ApplicationRequest(
      ApplicationRequester requester_value,
      uint32_t request_id_value = 0,
      ApplicationRequestKind kind_value = ApplicationRequestKind::kGetConfig)
      : requester(requester_value),
        request_id(request_id_value),
        kind(kind_value) {}

  ApplicationRequester requester;
  uint32_t request_id;
  ApplicationRequestKind kind;
};

struct ApplicationResponse {
  constexpr ApplicationResponse(
      ApplicationRequester requester_value = ApplicationRequester::kUsb,
      uint32_t request_id_value = 0,
      ApplicationResponseCode code_value = ApplicationResponseCode::kUnsupported,
      bool config_backend_ready_value = false,
      bool config_has_committed_record_value = false,
      config_format::Config config_value = config_format::Config())
      : requester(requester_value),
        request_id(request_id_value),
        code(code_value),
        config_backend_ready(config_backend_ready_value),
        config_has_committed_record(config_has_committed_record_value),
        config(config_value) {}

  ApplicationRequester requester;
  uint32_t request_id;
  ApplicationResponseCode code;

  // Meaningful for kGetConfig/kOk. ready() only means ConfigStore/backend
  // initialization succeeded; blank or corrupt flash can still legitimately
  // fall back to defaults. config_has_committed_record distinguishes a
  // recovered durable record from that default/fallback source.
  bool config_backend_ready;
  bool config_has_committed_record;
  config_format::Config config;
};

// One response slot is intentional backpressure: a transport must consume the
// prior result before submitting more work. This keeps memory fixed. M7P7E
// makes response ownership explicit: a mismatched requester cannot consume or
// clear the pending response. The rightful requester may explicitly discard
// its response (for example after a transport disconnect) so one abandoned
// result cannot wedge the global bounded slot forever.
class ApplicationRequestService {
 public:
  explicit ApplicationRequestService(ConfigStore& config_store)
      : config_store_(config_store), response_ready_(false), response_() {}

  ApplicationSubmitResult submit(const ApplicationRequest& request);
  bool takeResponse(ApplicationRequester requester, ApplicationResponse& response);
  bool discardResponse(ApplicationRequester requester);
  bool responsePending() const { return response_ready_; }

 private:
  ConfigStore& config_store_;
  bool response_ready_;
  ApplicationResponse response_;
};

}  // namespace orun_tlp
