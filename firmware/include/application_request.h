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
  uint32_t request_id = 0;
  ApplicationResponseCode code = ApplicationResponseCode::kUnsupported;

  // Meaningful for kGetConfig/kOk. ConfigStore deliberately reports its safe
  // fallback even when persistence is unavailable; config_store_ready keeps
  // callers from misrepresenting that fallback as a recovered durable value.
  bool config_store_ready = false;
  config_format::Config config{};
};

// One response slot is intentional backpressure: a transport must consume the
// prior result before submitting more work. This keeps memory fixed and makes
// queue ownership explicit before BLE framing exists.
class ApplicationRequestService {
 public:
  explicit ApplicationRequestService(ConfigStore& config_store)
      : config_store_(config_store) {}

  ApplicationSubmitResult submit(const ApplicationRequest& request);
  bool takeResponse(ApplicationResponse& response);
  bool responsePending() const { return response_ready_; }

 private:
  ConfigStore& config_store_;
  bool response_ready_ = false;
  ApplicationResponse response_{};
};

}  // namespace orun_tlp
