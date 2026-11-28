#pragma once

#include <stddef.h>
#include <stdint.h>

#include "gateway_custody_ack_scope.h"
#include "kq05_gateway_batch_custody_ack.h"
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

  // Only the current 0x0A protocol is supported.
  // Durable readback publishes an exact handle, never an ACK directly.
  Submit submit(const uint8_t* exact_object, size_t size,
                const GatewayCustodyAckScope& scope,
                const uint8_t (&scoped_key)[32],
                bool batch_mode = true);
  void service();
  bool waitingFlash() const { return waiting_flash_; }
  bool batchReady() const { return batch_ready_; }

 private:
  friend class GatewayCustodyAuthorizedIngress;
  // The EXISTING local custody owner is also the sole Gateway OCQ2 proof
  // authority for 0x0A; no second flash writer/collector may mint it.
  // Handles must already name verified, durably held exact objects.
  // Fail-closed: no caller output modification on failure.
  bool signDurableBatch(
      const ProductCustodyStore::Handle* handles, uint8_t count,
      const GatewayCustodyAckScope& scope, const uint8_t (&scoped_key)[32],
      uint8_t (&wire)[kq05_gateway_batch_ack::kMaxSize],
      size_t& written, bool* stale_single = nullptr);
  bool confirmCommitted(const ProductCustodyStore::Handle& handle);
  // Same-loop trusted ingress only; no public ability to obtain raw handles.
  bool takeBatchHandle(ProductCustodyStore::Handle& output);
  void clearPending();

  ProductCustodyStore& store_;
  bool waiting_flash_ = false;
  bool batch_ready_ = false;
  ProductCustodyStore::Handle batch_handle_{};
  size_t pending_size_ = 0U;
  uint8_t pending_object_[100]{};
};

}  // namespace orun_tlp
