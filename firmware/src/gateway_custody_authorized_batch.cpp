#include "gateway_custody_authorized_ingress.h"

#include <string.h>

#include "gateway_local_custody_owner.h"

namespace orun_tlp {
namespace {
void wipeBatch(void* data, size_t n) {
  volatile uint8_t* p = static_cast<volatile uint8_t*>(data);
  while (n--) *p++ = 0U;
}
bool equalKey(const uint8_t (&a)[32], const uint8_t (&b)[32]) {
  uint8_t diff = 0U;
  for (unsigned i = 0U; i < 32U; ++i)
    diff = static_cast<uint8_t>(diff | (a[i] ^ b[i]));
  return diff == 0U;
}
bool validScope(const GatewayCustodyAckScope& s, uint64_t gateway) {
  return gateway != 0U && s.tracker_device_id != 0U &&
         s.tracker_device_id != UINT64_MAX &&
         s.gateway_device_id == gateway &&
         s.key_epoch != UINT32_MAX &&
         s.custody_floor != UINT32_MAX &&
         s.grant_generation != 0U &&
         s.grant_generation != UINT32_MAX;
}
bool matches(const GatewayCustodyAckScope& a,
             const GatewayCustodyAckScope& b) {
  return a.tracker_device_id == b.tracker_device_id &&
         a.gateway_device_id == b.gateway_device_id &&
         a.key_epoch == b.key_epoch &&
         a.custody_floor == b.custody_floor &&
         a.grant_generation == b.grant_generation;
}
}  // namespace

bool GatewayCustodyAuthorizedIngress::prepareAuthorizedBatchAck(
    const ProductCustodyStore::Handle* handles, uint8_t count,
    const GatewayCustodyAckScope& requested,
    uint8_t (&wire)[kq05_gateway_batch_ack::kMaxSize],
    size_t& written) {
  if (pending_ || custody_.waitingFlash() || custody_.ackReady() ||
      handles == nullptr || count == 0U ||
      count > kq05_gateway_batch_ack::kMaxObjects ||
      !validScope(requested, gateway_id_)) return false;

  GatewayCustodyAckScope current{};
  uint8_t key[32]{};
  const bool authorized = grants_.activeGrant(
      requested.tracker_device_id, requested.key_epoch, current, key);
  if (!authorized || !validScope(current, gateway_id_) ||
      !matches(requested, current)) {
    wipeBatch(key, sizeof(key));
    return false;
  }

  uint8_t candidate[kq05_gateway_batch_ack::kMaxSize]{};
  size_t candidate_size = 0U;
  const bool durable = custody_.signDurableBatch(
      handles, count, current, key, candidate, candidate_size);

  // Just like the old 0x05 path, grant revocation after durable admission
  // suppresses any outbound acknowledgement. Compare the entire key too:
  // its immutability for an unchanged scope is an enforceable invariant.
  GatewayCustodyAckScope latest{};
  uint8_t later_key[32]{};
  const bool still_active = durable && grants_.activeGrant(
      requested.tracker_device_id, requested.key_epoch, latest, later_key);
  const bool approved = durable && still_active &&
      validScope(latest, gateway_id_) && matches(current, latest) &&
      equalKey(key, later_key);
  if (approved) {
    memcpy(wire, candidate, candidate_size);
    written = candidate_size;
  }
  wipeBatch(later_key, sizeof(later_key));
  wipeBatch(key, sizeof(key));
  wipeBatch(candidate, sizeof(candidate));
  return approved;
}
}  // namespace orun_tlp
