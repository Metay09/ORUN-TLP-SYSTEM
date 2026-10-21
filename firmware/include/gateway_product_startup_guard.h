#pragma once

#include <stdint.h>

#include "flash_backend.h"
#include "gateway_custody_authorized_ingress.h"
#include "product_custody_store.h"
#include "product_region.h"

namespace orun_tlp {

// A FUTURE trusted Gateway enrollment store must implement this interface.
// The method is true only after LOCAL, persistent provisioning has been
// authenticated and recovered at boot. Never infer it from BASE role, a
// received frame, Tracker K_root, or an in-memory test credential.
class GatewayDurableGrantSource : public GatewayCustodyGrantProvider {
 public:
  virtual bool verifiedForLocalGateway(uint64_t gateway_id) const = 0;
};

// SF5K3X: boot-only, fail-closed OCQ2 owner admission. No erase, migration,
// enrollment, gateway activation, secrets, or radio callbacks.
class GatewayProductStartupGuard final {
 public:
  enum class State : uint8_t {
    kNotChecked, kServiceDisabled, kInvalidIdentity, kTrackerOwnsRegion,
    kUnprovisioned, kFlashUnavailable, kForeignRegion,
    kNeedsPreparation, kMountRejected, kReady
  };

  GatewayProductStartupGuard(FlashBackend& flash, uint16_t page_count,
                             ProductCustodyStore& custody,
                             const GatewayDurableGrantSource& grants,
                             uint64_t gateway_id)
      : flash_(flash), page_count_(page_count), custody_(custody),
        grants_(grants), gateway_id_(gateway_id) {}

  // Call ONCE, on a dedicated Gateway boot before Bluefruit.begin().
  // Both boolean inputs must come from trusted application ownership, NEVER
  // the legacy BASE role or untrusted RF. Rejected boot remains rejected.
  State inspectAndMount(bool authorized_gateway_service,
                        bool observation_store_mounted,
                        uint64_t hardware_device_id);

  // Re-evaluate enrollment and application authority on EVERY RX/ACK pass;
  // no stale boot-time authorization survives a grant revocation.
  bool mayServe(bool authorized_gateway_service,
                bool observation_store_mounted,
                uint64_t live_radio_device_id) const;

  State state() const { return state_; }

 private:
  FlashBackend& flash_;
  uint16_t page_count_;
  ProductCustodyStore& custody_;
  const GatewayDurableGrantSource& grants_;
  uint64_t gateway_id_;
  State state_ = State::kNotChecked;
};

}  // namespace orun_tlp
