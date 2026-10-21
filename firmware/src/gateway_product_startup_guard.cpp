#include "gateway_product_startup_guard.h"

namespace orun_tlp {

GatewayProductStartupGuard::State
GatewayProductStartupGuard::inspectAndMount(
    bool authorized_gateway_service, bool observation_store_mounted,
    uint64_t hardware_device_id) {
  // No retry in one boot, including after an unsuccessful census/mount.
  // A new authorization, ownership change or repaired flash needs reboot
  // through the same controlled pre-SoftDevice path.
  if (state_ != State::kNotChecked) return state_;

  if (!authorized_gateway_service)
    return state_ = State::kServiceDisabled;
  if (gateway_id_ == 0U || hardware_device_id == 0U ||
      hardware_device_id != gateway_id_)
    return state_ = State::kInvalidIdentity;
  if (observation_store_mounted)
    return state_ = State::kTrackerOwnsRegion;
  if (!grants_.verifiedForLocalGateway(gateway_id_))
    return state_ = State::kUnprovisioned;
  if (page_count_ == 0U || page_count_ > 32U ||
      !flash_.begin() || flash_.hasUnreconciledMutation())
    return state_ = State::kFlashUnavailable;

  product_region::Census census{};
  if (!product_region::takeCensus(flash_, page_count_, census))
    return state_ = State::kFlashUnavailable;
  const auto decision = product_region::mountProductCustodyDecision(census);
  if (decision == product_region::MountDecision::kForeignStore)
    return state_ = State::kForeignRegion;
  if (decision != product_region::MountDecision::kMount)
    return state_ = State::kNeedsPreparation;

  // ProductCustodyStore performs its independent exact OCQ2 journal scan,
  // power-cut classification and flash readback without writing at boot.
  if (!custody_.begin())
    return state_ = State::kMountRejected;
  return state_ = State::kReady;
}

bool GatewayProductStartupGuard::mayServe(
    bool authorized_gateway_service, bool observation_store_mounted,
    uint64_t live_radio_device_id) const {
  return state_ == State::kReady && authorized_gateway_service &&
         !observation_store_mounted &&
         gateway_id_ != 0U && live_radio_device_id == gateway_id_ &&
         grants_.verifiedForLocalGateway(gateway_id_) &&
         custody_.ready() && !custody_.busy() &&
         !flash_.hasUnreconciledMutation();
}

}  // namespace orun_tlp
