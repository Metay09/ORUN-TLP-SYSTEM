#pragma once

#include <stddef.h>
#include <stdint.h>

#include "gateway_custody_authorized_ingress.h"
#include "product_custody_store.h"

namespace orun_tlp {

// KQ05 experimental Gateway LIVE intake. Authenticated Gateway provisioning
// grants only admission to the opaque durable staging store; a grant does NOT
// prove the sender's AEAD plaintext or authorize a delivery receipt.
//
// This class NEVER signs/transmits a receipt, calls a release primitive,
// accepts a custody-requested object, or erases records. The backend still
// must authenticate/decrypt the PRODUCT_SECURE frame before it is credited.
class GatewayProductLiveIngress final {
 public:
  enum class Receive : uint8_t { kStarted, kDuplicateDurable, kBusy, kDenied };
  enum class Service : uint8_t { kNone, kDurable, kFailed };

  GatewayProductLiveIngress(ProductCustodyStore& store,
                            GatewayCustodyGrantProvider& grants,
                            uint64_t gateway_id)
      : store_(store), grants_(grants), gateway_id_(gateway_id) {}

  Receive receive(const uint8_t* frame, size_t size);
  Service service();
  bool pending() const { return pending_; }
  bool storageBusy() const {
    return pending_ || store_.busy() || store_.hasOutcome();
  }
  uint64_t gatewayId() const { return gateway_id_; }

 private:
  bool readBackMatches(const ProductCustodyStore::Handle& handle,
                       const uint8_t* frame, size_t size) const;
  void reset();

  ProductCustodyStore& store_;
  GatewayCustodyGrantProvider& grants_;
  uint64_t gateway_id_;
  bool pending_ = false;
  uint8_t pending_object_[custody_store_product_format::kMaxObjectSize]{};
  size_t pending_size_ = 0U;
};

}  // namespace orun_tlp
