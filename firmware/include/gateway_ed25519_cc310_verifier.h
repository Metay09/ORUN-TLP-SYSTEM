#pragma once

#include "gateway_grant_snapshot_reader.h"

// SF5K3AA: target-CRYPTO KAT adapter. The default firmware deliberately
// has NO issuer trust anchor, Gateway enrollment, or authorized runtime.
// Compilation without one of these flags must not expose a live verifier.
// Step 3B: the bench Gateway image (ORUN_STEP3B_GATEWAY_ENROLLMENT) uses it
// with the issuer public key pinned at build time.
#if defined(ORUN_SF5K3AA_TARGET_CRYPTO_PROBE) || \
    defined(ORUN_STEP3B_GATEWAY_ENROLLMENT)
namespace orun_tlp {

// The caller MUST supply a provisioned, independently trusted immutable
// issuer public key. Never derive this from received RF, OGK1, BLE or a USB
// command. This class authenticates a signature; it does not stop rollback.
class GatewayEd25519Cc310Verifier final
    : public GatewayEnrollmentProofVerifier {
 public:
  explicit GatewayEd25519Cc310Verifier(
      const uint8_t (&trusted_issuer_public_key)[32]);
  bool verify(
      const uint8_t* canonical_payload, size_t payload_size,
      const uint8_t (&signature)[gateway_grant_format::kProofBytes])
      const override;

 private:
  uint8_t issuer_public_key_[32]{};
};

}  // namespace orun_tlp
#endif
