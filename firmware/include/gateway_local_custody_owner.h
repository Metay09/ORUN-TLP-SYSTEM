#pragma once

#include <stddef.h>
#include <stdint.h>

#include "gateway_custody_ack_signer.h"
#include "product_custody_store.h"

namespace orun_tlp {

// SF5K3P: the ONLY local-flash caller of the privileged ACK signer.
// NO claimed Edge/bypass authority. RF cannot generate an ACK directly.
// Enrollment, admission authentication and scoped-key provisioning are NOT
// supplied here; the future trusted Gateway composition owner must provide
// an actually-authorized scope/key and serialized RX access.
class GatewayLocalCustodyOwner final {
 public:
  enum class Submit : uint8_t { kStarted, kAckReady, kBusy, kRejected };
  explicit GatewayLocalCustodyOwner(ProductCustodyStore& store) : store_(store) {}
  ~GatewayLocalCustodyOwner();
  GatewayLocalCustodyOwner(const GatewayLocalCustodyOwner&) = delete;
  GatewayLocalCustodyOwner& operator=(const GatewayLocalCustodyOwner&) = delete;

  Submit submit(const uint8_t* exact_object, size_t size,
                const GatewayCustodyAckScope& scope,
                const uint8_t (&scoped_key)[32]);
  void service();
  bool takeAck(uint8_t (&output)[custody_ack::kWireSize]);
  bool waitingFlash() const { return waiting_flash_; }
  bool ackReady() const { return ack_ready_; }

 private:
  bool signCommitted(const ProductCustodyStore::Handle& handle);
  void clearPending();

  ProductCustodyStore& store_;
  bool waiting_flash_ = false;
  bool ack_ready_ = false;
  size_t pending_size_ = 0U;
  GatewayCustodyAckScope pending_scope_{};
  uint8_t pending_object_[100]{};
  uint8_t pending_key_[32]{};
  uint8_t signed_ack_[custody_ack::kWireSize]{};
};

}  // namespace orun_tlp
