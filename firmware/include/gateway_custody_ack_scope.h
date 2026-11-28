#pragma once

#include <stdint.h>

namespace orun_tlp {

// Authorized custody grant scope; no secret key or wire protocol.
struct GatewayCustodyAckScope {
  uint64_t tracker_device_id = 0U;
  uint64_t gateway_device_id = 0U;
  uint32_t key_epoch = UINT32_MAX;
  uint32_t custody_floor = UINT32_MAX;
  uint32_t grant_generation = 0U;
};

}  // namespace orun_tlp
