#pragma once

#include <stdint.h>

#include "config_format.h"

namespace orun_tlp {

class ConfigStore;

// M7P7D: typed, transport-neutral application request seam.
//
// This is deliberately NOT a wire format. USB/BLE/other adapters translate
// their bounded transport input into one of these typed requests. The request
// service remains loop-task owned; BLE callbacks must never call it directly.
//
// M7P7D exposes only one safe, read-only operation. Protected config mutation,
// provisioning, MESSAGE and command/control requests remain later work behind
// their reviewed authorization/security gates.
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
};

struct ApplicationRequest {
  constexpr ApplicationRequest(
      uint32_t request_id_value = 0,
      ApplicationRequestKind kind_value = ApplicationRequestKind::kGetConfig)
      : request_id(request_id_value), kind(kind_value) {}

  uint32_t request_id;
  ApplicationRequestKind kind;
};

struct ApplicationResponse {
  constexpr ApplicationResponse(
      uint32_t request_id_value = 0,
      ApplicationResponseCode code_value = ApplicationResponseCode::kUnsupported,
      bool config_backend_ready_value = false,
      bool config_has_committed_record_value = false,
      config_format::Config config_value = config_format::Config())
      : request_id(request_id_value),
        code(code_value),
        config_backend_ready(config_backend_ready_value),
        config_has_committed_record(config_has_committed_record_value),
        config(config_value) {}

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
// prior result before submitting more work. This keeps memory fixed and makes
// queue ownership explicit before BLE framing exists.
class ApplicationRequestService {
 public:
  explicit ApplicationRequestService(ConfigStore& config_store)
      : config_store_(config_store), response_ready_(false), response_() {}

  ApplicationSubmitResult submit(const ApplicationRequest& request);
  bool takeResponse(ApplicationResponse& response);
  bool responsePending() const { return response_ready_; }

 private:
  ConfigStore& config_store_;
  bool response_ready_;
  ApplicationResponse response_;
};

}  // namespace orun_tlp
