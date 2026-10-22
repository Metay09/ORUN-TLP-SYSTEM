#include "gateway_anchored_grant_source.h"

namespace orun_tlp {
namespace {
void wipe(void* ptr, size_t size) {
  volatile uint8_t* p = static_cast<volatile uint8_t*>(ptr);
  while (size-- != 0U) *p++ = 0U;
}
}  // namespace

bool GatewayAnchoredGrantSource::verifiedForLocalGateway(
    uint64_t gateway_id) const {
  if (!snapshot_.verifiedForLocalGateway(gateway_id)) return false;
  uint64_t authoritative = 0U;
  return anchor_.currentRevision(gateway_id, authoritative) &&
         authoritative != 0U &&
         authoritative == snapshot_.activeGeneration() &&
         snapshot_.verifiedForLocalGateway(gateway_id);
}

bool GatewayAnchoredGrantSource::activeGrant(
    uint64_t tracker_id, uint32_t key_epoch, GatewayCustodyAckScope& scope,
    uint8_t (&scoped_key)[32]) {
  const uint64_t local = snapshot_.localGatewayId();
  if (local == 0U || !verifiedForLocalGateway(local)) return false;

  // Take all candidate output into scratch so the user-provided buffers stay
  // unchanged if the independent anchor is revoked during the lookup.
  GatewayCustodyAckScope candidate_scope{};
  uint8_t candidate_key[32]{};
  const bool accepted = snapshot_.activeGrant(tracker_id, key_epoch,
                                              candidate_scope, candidate_key);
  if (!accepted || !verifiedForLocalGateway(local) ||
      candidate_scope.gateway_device_id != local ||
      candidate_scope.tracker_device_id != tracker_id ||
      candidate_scope.key_epoch != key_epoch) {
    wipe(candidate_key, sizeof(candidate_key));
    return false;
  }
  scope = candidate_scope;
  for (size_t i = 0U; i < sizeof(candidate_key); ++i)
    scoped_key[i] = candidate_key[i];
  wipe(candidate_key, sizeof(candidate_key));
  return true;
}

}  // namespace orun_tlp
