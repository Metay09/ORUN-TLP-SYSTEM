#pragma once

#include <stddef.h>
#include <stdint.h>

#include "custody_ack.h"

namespace orun_tlp {

class GatewayLocalCustodyOwner;
class GatewayEdgeDurableAcceptOwner;

// This is an already-authorized per-tracker/per-gateway custody-ACK scope.
// The ACK key itself is deliberately NOT part of the scope, nor is K_root.
// Its associated K_custody_ack is supplied separately by the trusted owner.
struct GatewayCustodyAckScope {
  uint64_t tracker_device_id = 0U;
  uint64_t gateway_device_id = 0U;
  uint32_t key_epoch = UINT32_MAX;
  uint32_t custody_floor = UINT32_MAX;
  uint32_t grant_generation = 0U;
};

// Wire-only signer for one exact protected inner object. No ordinary public
// invocation exists: only the two future durability evidence owners may mint
// an ACK. The friends MUST first independently establish either:
//   (a) local flash commit + successful completion + exact readback, OR
//   (b) authenticated exact-object EDGE_DURABLE_ACCEPT.
// A radio RX, TX_DONE, RAM queueing or mere link availability is NOT evidence.
// This component does not itself implement those authorities, flash, or RF.
class GatewayCustodyAckSigner final {
 public:
  GatewayCustodyAckSigner() = delete;

 private:
  friend class GatewayLocalCustodyOwner;
  friend class GatewayEdgeDurableAcceptOwner;
#ifdef ORUN_SF5K3N_HOST_TEST
  friend struct GatewayCustodyAckSignerTestPeer;
#endif
  // On failure, output remains byte-identical. The key is the *already
  // derived* 32-byte K_custody_ack for precisely this durable scope, NOT root.
  static bool signAfterDurableAcceptance(
      const uint8_t* exact_object, size_t object_len,
      const GatewayCustodyAckScope& scope,
      const uint8_t (&custody_ack_key)[32],
      uint8_t (&ack_out)[custody_ack::kWireSize]);
};

}  // namespace orun_tlp
