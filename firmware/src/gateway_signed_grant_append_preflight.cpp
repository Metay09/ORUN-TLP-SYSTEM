#include "gateway_signed_grant_append_preflight.h"

#include <string.h>

namespace orun_tlp {
namespace {
namespace fmt = gateway_grant_format;

bool allZero(const uint8_t* p, size_t count) {
  uint8_t any = 0U;
  for (size_t i = 0U; i < count; ++i) any |= p[i];
  return any == 0U;
}
}  // namespace

GatewaySignedGrantAppendPreflight::Verdict
GatewaySignedGrantAppendPreflight::inspect(
    const GatewayGrantSnapshotReader& current,
    const GatewayGrantSnapshotReader& candidate,
    const GatewayGrantRevisionAnchor& anchor,
    uint64_t gateway) {
  typedef GatewaySignedGrantAppendPreflight::Verdict V;

  if (gateway == 0U || gateway == UINT64_MAX ||
      current.localGatewayId() != gateway ||
      candidate.localGatewayId() != gateway)
    return V::kWrongGateway;

  if (!current.verifiedForLocalGateway(gateway))
    return V::kCurrentUnverified;
  if (!candidate.verifiedForLocalGateway(gateway))
    return V::kCandidateUnverified;

  const uint64_t generation = current.activeGeneration();
  const uint64_t requested = candidate.activeGeneration();
  if (generation == 0U || generation == UINT64_MAX ||
      requested == 0U || requested == UINT64_MAX)
    return V::kRevisionExhausted;

  // The current committed snapshot must exactly match an INDEPENDENT
  // authenticated checkpoint. An old valid signature is insufficient.
  uint64_t revision = 0U;
  if (!anchor.currentRevision(gateway, revision) || revision == 0U)
    return V::kAnchorUnavailable;
  if (revision != generation)
    return V::kAnchorMismatch;
  if (requested != generation + 1U)
    return V::kNotNextRevision;

  bool added = false;
  for (size_t slot = 0U; slot < fmt::kGrantSlots; ++slot) {
    const uint8_t* before = current.scratch_ +
        fmt::kHeaderBytes + slot * fmt::kGrantBytes;
    const uint8_t* after = candidate.scratch_ +
        fmt::kHeaderBytes + slot * fmt::kGrantBytes;

    if (!allZero(before, fmt::kGrantBytes)) {
      // Retain every byte including exact scoped HMAC key, epoch,
      // grant generation and floor. No silent mutation or reordering.
      if (memcmp(before, after, fmt::kGrantBytes) != 0)
        return V::kExistingGrantChanged;
    } else if (!allZero(after, fmt::kGrantBytes)) {
      added = true;
    }
  }
  if (!added) return V::kNoNewTracker;

  // Detect volatile role, grant-reader or anchor changes DURING review.
  // No key/public authority is published by this informational result.
  uint64_t final_revision = 0U;
  if (!anchor.currentRevision(gateway, final_revision) ||
      !current.verifiedForLocalGateway(gateway) ||
      !candidate.verifiedForLocalGateway(gateway) ||
      current.activeGeneration() != generation ||
      candidate.activeGeneration() != requested ||
      final_revision != generation)
    return V::kAnchorMismatch;

  return V::kEligibleForSeparateCommitReview;
}

}  // namespace orun_tlp
