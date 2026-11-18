#pragma once
#include <stddef.h>
#include <stdint.h>

#include "gateway_custody_authorized_ingress.h"
#include "kq05_gateway_batch_custody_ack.h"
#include "product_custody_store.h"

namespace orun_tlp {

// Gateway-only, synchronous loop-task collector. Never call from LoRa RX/ISR.
// Handles are pointers to objects ALREADY committed by ProductCustodyStore,
// never to a pending radio frame. No queue limit of four is imposed here;
// this class reads back at most five immutable records directly from Flash.
// This slice DOES NOT add an RF TX path or Tracker release logic.
class Kq05GatewayDurableBatchOwner final {
 public:
  enum class Result : uint8_t {
    kReady,      // authenticated bytes prepared, NOT transmitted
    kDenied,     // wrong identity, grant, scope, duplicate or bad shape
    kBusy,       // Flash transition or unconsumed storage outcome
    kNotDurable  // missing/torn/foreign committed object
  };

  Kq05GatewayDurableBatchOwner(ProductCustodyStore& store,
                              GatewayCustodyGrantProvider& grants,
                              uint64_t local_gateway_id)
      : store_(store), grants_(grants), gateway_id_(local_gateway_id) {}

  // Serialise with grant mutations, ProductCustodyStore::poll() and
  // every maintenance / page reclaim. No output or length mutation on failure.
  // A caller must separately prove the local Gateway's trusted physical
  // identity and role before constructing this owner.
  Result issue(uint64_t tracker_id, uint32_t key_epoch,
               const ProductCustodyStore::Handle* exact_handles,
               uint8_t count,
               uint8_t (&wire_out)[kq05_gateway_batch_ack::kMaxSize],
               size_t& wire_length);

 private:
  ProductCustodyStore& store_;
  GatewayCustodyGrantProvider& grants_;
  const uint64_t gateway_id_;
};

}  // namespace orun_tlp
