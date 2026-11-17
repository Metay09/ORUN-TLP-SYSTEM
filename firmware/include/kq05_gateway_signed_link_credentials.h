#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef ORUN_KQ05_EXPERIMENTAL_ACKLESS_LIVE

#include "gateway_grant_snapshot_reader.h"
#include "kq05_gateway_ram_relay.h"

namespace orun_tlp::kq05_ram_relay {

// Bridges only a verified issuer-signed OGK1 v2 Gateway snapshot.
// The trusted installation ID is NOT read from an Edge BLE packet. It must
// come from an independently enrolled Edge identity in a later integration.
// No shared test key, Tracker K_root, Flash writer, or fallback is provided.
class SignedGrantCredentials final : public CredentialSource {
 public:
  SignedGrantCredentials(GatewayGrantSnapshotReader& grants,
                         uint64_t gateway_id, const char* trusted_edge_id);

  bool verifiedForGateway(uint64_t gateway_id) const override;
  bool readGatewayLink(uint64_t gateway_id, char (&edge_id)[65],
                       uint8_t (&link_key)[32]) const override;
  bool permitsTracker(uint64_t gateway_id, uint64_t tracker_id,
                      uint32_t key_epoch) const override;

 private:
  GatewayGrantSnapshotReader& grants_;
  uint64_t gateway_id_ = 0U;
  char edge_id_[65]{};
  bool valid_identity_ = false;
};

}  // namespace orun_tlp::kq05_ram_relay

#endif
