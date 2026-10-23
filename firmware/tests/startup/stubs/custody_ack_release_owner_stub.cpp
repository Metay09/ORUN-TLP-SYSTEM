// STARTUP HOST FIXTURE ONLY. Never link this into production firmware.
//
// The startup test compiles the actual setup()/loop() with fake peripherals;
// it has no custody-key provisioning or CC310 crypto implementation.
// Fail closed rather than forging a custody release. The real
// CustodyAckReleaseOwner and its HMAC + flash behavior are exercised by
// the independent SF5K3M host test and linked by the RAK4630 target.
#include "custody_ack_release_owner.h"

namespace orun_tlp {

CustodyAckReleaseOwner::Submit CustodyAckReleaseOwner::submit(
    const uint8_t*, size_t) {
  return Submit::kAuthenticationRejected;
}

CustodyAckReleaseOwner::Service CustodyAckReleaseOwner::service() {
  return Service::kIdle;
}

}  // namespace orun_tlp
