#pragma once

#include <stdint.h>

#include "gateway_product_startup_guard.h"
#include "gateway_grant_snapshot_reader.h"

namespace orun_tlp {

// Trusted *separate* monotonic issuer-revision checkpoint. Implementations
// must guarantee authentication, persistence across power loss and resistance
// to rollback/replacement; a normal read of the same OGK1 flash pages is NOT
// independent evidence. No production anchor is provided in SF5K3Z.
class GatewayGrantRevisionAnchor {
 public:
  virtual ~GatewayGrantRevisionAnchor() = default;
  // Return the EXACT enrolled revision, not merely a lower bound. Reject
  // unset/ambiguous/stale evidence by returning false.
  virtual bool currentRevision(uint64_t gateway_id,
                               uint64_t& revision) const = 0;
};

// Fail-closed admission bridge: both the signed snapshot and independent
// rollback-resistant authority must agree on exactly one revision. In a
// future authenticated re-enrollment flow the new snapshot and the protected
// anchor require a reviewed transaction/boot policy; no such writer exists.
class GatewayAnchoredGrantSource final : public GatewayDurableGrantSource {
 public:
  GatewayAnchoredGrantSource(GatewayGrantSnapshotReader& snapshot,
                             const GatewayGrantRevisionAnchor& anchor)
      : snapshot_(snapshot), anchor_(anchor) {}

  bool verifiedForLocalGateway(uint64_t gateway_id) const override;
  bool activeGrant(uint64_t tracker_id, uint32_t key_epoch,
                   GatewayCustodyAckScope& scope,
                   uint8_t (&scoped_key)[32]) override;

 private:
  GatewayGrantSnapshotReader& snapshot_;
  const GatewayGrantRevisionAnchor& anchor_;
};

}  // namespace orun_tlp
