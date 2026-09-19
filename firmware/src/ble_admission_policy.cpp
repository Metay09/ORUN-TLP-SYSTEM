#include "ble_admission_policy.h"

#include "monotonic_time.h"

namespace orun_tlp {

void BleAdmissionPolicy::begin(uint32_t now) {
  open_ = true;
  connected_ = false;
  close_at_ms_ = now + ble_admission_config::kNoClientTimeoutMs;
}

BleAdmissionAction BleAdmissionPolicy::update(bool connected, uint32_t now) {
  if (!open_) {
    // Already closed. Do not reopen implicitly -- a future explicit,
    // authenticated reopen path (e.g. LoRa OPEN_BLE) is out of scope for
    // this slice; see docs/milestones/M7P7B.md.
    connected_ = false;
    return BleAdmissionAction::kNone;
  }

  if (connected) {
    // Never closes while a client is connected, regardless of how close the
    // window was to expiring.
    connected_ = true;
    return BleAdmissionAction::kNone;
  }

  if (connected_) {
    // Edge: was connected last tick, is not now. Open exactly one fresh
    // bounded window -- repeated connect/disconnect cycles each grant one
    // more bounded window, never an unbounded/permanent one.
    connected_ = false;
    close_at_ms_ = now + ble_admission_config::kNoClientTimeoutMs;
    return BleAdmissionAction::kNone;
  }

  if (!monotonic::reached(now, close_at_ms_)) return BleAdmissionAction::kNone;

  open_ = false;
  return BleAdmissionAction::kClose;
}

}  // namespace orun_tlp
