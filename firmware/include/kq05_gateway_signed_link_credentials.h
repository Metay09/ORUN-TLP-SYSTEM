#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef ORUN_KQ05_EXPERIMENTAL_ACKLESS_LIVE

#include "gateway_grant_snapshot_reader.h"
#include "kq05_gateway_ram_relay.h"

namespace orun_tlp::kq05_ram_relay {

// Must be implemented ONLY by an independently authenticated, locally
// recovered enrollment owner. No BLE packet, GATT connection name, app
// supplied string or unverified device role can implement this authority.
// There is intentionally NO production owner in this experimental slice.
class TrustedEdgeInstallation {
 public:
  virtual ~TrustedEdgeInstallation() = default;
  // Output must be an ASCII installation ID (1..64); FALSE on unavailable,
  // revoked, wrong Gateway, rollback, or unverified enrollment.
  virtual bool readVerifiedInstallation(uint64_t gateway_id,
                                        char (&installation_id)[65]) const = 0;
};

// Bridges only a verified issuer-signed OGK1 v2 Gateway snapshot AND a
// separately authorized Edge installation. Never use a caller supplied
// string as an Edge identity. No Tracker K_root, writer or test fallback.
class SignedGrantCredentials final : public CredentialSource {
 public:
  SignedGrantCredentials(GatewayGrantSnapshotReader& grants,
                         uint64_t gateway_id,
                         const TrustedEdgeInstallation& installation);

  bool verifiedForGateway(uint64_t gateway_id) const override;
  bool readGatewayLink(uint64_t gateway_id, char (&edge_id)[65],
                       uint8_t (&link_key)[32]) const override;
  bool permitsTracker(uint64_t gateway_id, uint64_t tracker_id,
                      uint32_t key_epoch) const override;

 private:
  bool enrolledIdentity(char (&installation_id)[65]) const;
  GatewayGrantSnapshotReader& grants_;
  uint64_t gateway_id_ = 0U;
  const TrustedEdgeInstallation& installation_;
};

}  // namespace orun_tlp::kq05_ram_relay

#endif
