#pragma once

#include <stdint.h>

#include "gateway_anchored_grant_source.h"

namespace orun_tlp {

// SF5K3AB: pure READ-ONLY review of an off-device issuer-signed OGK1
// proposal before any future authenticated onboarding transaction.
// Does NOT authorize flash writing, Gateway ACK, key installation,
// issuer trust provisioning or persistent rollback-anchor advancement.
//
// Limited to ADDING Tracker grants in previously empty slots. Revocation,
// key rotation, same-Tracker epoch changes and slot reordering require a
// separately reviewed authenticated transaction and are deliberately denied.
class GatewaySignedGrantAppendPreflight final {
 public:
  enum class Verdict : uint8_t {
    kCurrentUnverified,
    kCandidateUnverified,
    kIssuerAuthorityMismatch,
    kWrongGateway,
    kAnchorUnavailable,
    kAnchorMismatch,
    kRevisionExhausted,
    kNotNextRevision,
    kExistingGrantChanged,
    kNoNewTracker,
    kEligibleForSeparateCommitReview
  };

  // Both readers must have completed *real issuer-authenticated* recovery
  // using a trusted verifier before this call. The candidate may reside
  // in an UNTRUSTED staging medium, never in active SecurityStore pages.
  // Caller must separately attest that the anchor is truly monotonic,
  // independent and trusted. Test fake anchors are never production authority.
  static Verdict inspect(
      const GatewayGrantSnapshotReader& current,
      const GatewayGrantSnapshotReader& candidate,
      const GatewayGrantRevisionAnchor& anchor,
      uint64_t trusted_local_gateway_id);
};

}  // namespace orun_tlp
