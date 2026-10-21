#pragma once

#include <stdint.h>

#include "gateway_custody_authorized_ingress.h"
#include "radio_manager.h"

namespace orun_tlp {

// Gateway application-loop glue for the EXISTING custody owner and RF owner.
// The trusted composition root must independently mount an OCQ2-only product
// region and provide durable, provisioned GatewayCustodyGrantProvider grants.
// This class neither provisions keys nor grants Gateway authority to BASE.
class GatewayProductCustodyRuntime final {
 public:
  enum class Event : uint8_t {
    kNone, kCustodyStarted, kDuplicateHeld, kDenied, kAckTxStarted
  };

  GatewayProductCustodyRuntime(RadioManager& radio,
                               GatewayCustodyAuthorizedIngress& ingress)
      : radio_(radio), ingress_(ingress) {}

  // Call after RadioManager::update() in the SAME loop task as grant mutation.
  // enabled must be granted by an authenticated Gateway service owner, NEVER
  // derived solely from NodeRole::kBase or reception of a radio packet.
  // Pending flash completions still progress when service is disabled;
  // disabled/mismatched-identity instances never receive or transmit.
  Event update(bool enabled);

 private:
  RadioManager& radio_;
  GatewayCustodyAuthorizedIngress& ingress_;
};

}  // namespace orun_tlp
