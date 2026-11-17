#include "kq05_gateway_signed_link_credentials.h"

#ifdef ORUN_KQ05_EXPERIMENTAL_ACKLESS_LIVE

#include <string.h>

#include "custody_ack_primitives.h"

namespace orun_tlp::kq05_ram_relay {
namespace {

// Domain separation: OGK1 v2 Edge ACCEPT key is an input key ONLY, never
// reused directly as the KQ05 offer HMAC key. Edge must independently derive
// exactly the same 32B key from its trusted enrollment material.
constexpr char kDeriveDomain[] = "ORUN-KQ05-OGK1-EDGE-LINK-DERIVE-v1";
constexpr size_t kMaxDeriveInput = sizeof(kDeriveDomain) + 8U + 64U;
static_assert(kMaxDeriveInput <= custody_crypto::kMaxBoundedData,
              "KQ05 derived Edge key exceeds bounded crypto input");

void wipe(void* data, size_t size) {
  volatile uint8_t* p = static_cast<volatile uint8_t*>(data);
  while (size-- != 0U) *p++ = 0U;
}

bool validId(const char* candidate, char (&output)[65]) {
  memset(output, 0, sizeof(output));
  if (candidate == nullptr) return false;
  size_t i = 0U;
  for (; i < 65U; ++i) {
    const unsigned char ch = static_cast<unsigned char>(candidate[i]);
    if (ch == 0U) return i != 0U;
    if (i == 64U ||
        !((ch >= 'A' && ch <= 'Z') ||
          (ch >= 'a' && ch <= 'z') ||
          (ch >= '0' && ch <= '9') ||
          ch == '_' || ch == '-')) {
      wipe(output, sizeof(output));
      return false;
    }
    output[i] = static_cast<char>(ch);
  }
  wipe(output, sizeof(output));
  return false;
}

void put64(uint64_t number, uint8_t* out) {
  for (uint8_t i = 0U; i < 8U; ++i)
    out[i] = static_cast<uint8_t>(number >> (56U - 8U * i));
}

}  // namespace

SignedGrantCredentials::SignedGrantCredentials(
    GatewayGrantSnapshotReader& grants, uint64_t gateway_id,
    const char* trusted_edge_id)
    : grants_(grants), gateway_id_(gateway_id) {
  valid_identity_ =
      gateway_id != 0U && gateway_id != UINT64_MAX &&
      validId(trusted_edge_id, edge_id_);
}

bool SignedGrantCredentials::verifiedForGateway(uint64_t gateway_id) const {
  if (!valid_identity_ || gateway_id != gateway_id_ ||
      !grants_.verifiedForLocalGateway(gateway_id))
    return false;
  uint8_t source_key[32]{};
  const bool allowed = grants_.edgeAcceptKey(source_key);
  wipe(source_key, sizeof(source_key));
  return allowed; // OGK1 v1: valid custody grants but NO Edge authority.
}

bool SignedGrantCredentials::readGatewayLink(
    uint64_t gateway_id, char (&edge_id)[65],
    uint8_t (&link_key)[32]) const {
  memset(edge_id, 0, sizeof(edge_id));
  wipe(link_key, sizeof(link_key));
  if (!verifiedForGateway(gateway_id)) return false;

  uint8_t master[32]{};
  uint8_t data[kMaxDeriveInput]{};
  uint8_t derived[32]{};
  bool success = false;
  do {
    if (!grants_.edgeAcceptKey(master)) break;
    const size_t id_size = strlen(edge_id_);
    memcpy(data, kDeriveDomain, sizeof(kDeriveDomain));
    put64(gateway_id, data + sizeof(kDeriveDomain));
    memcpy(data + sizeof(kDeriveDomain) + 8U, edge_id_, id_size);
    const size_t n = sizeof(kDeriveDomain) + 8U + id_size;
    if (!custody_crypto::hmacSha256Bounded(master, data, n, derived))
      break;
    // Signed authority can be revoked while CC310 is running.
    if (!grants_.verifiedForLocalGateway(gateway_id)) break;
    memcpy(link_key, derived, sizeof(derived));
    memcpy(edge_id, edge_id_, id_size);
    success = true;
  } while (false);
  wipe(master, sizeof(master));
  wipe(data, sizeof(data));
  wipe(derived, sizeof(derived));
  return success;
}

bool SignedGrantCredentials::permitsTracker(
    uint64_t gateway_id, uint64_t tracker_id, uint32_t key_epoch) const {
  if (!verifiedForGateway(gateway_id) || tracker_id == 0U ||
      tracker_id == gateway_id || key_epoch == UINT32_MAX)
    return false;
  GatewayCustodyAckScope scope{};
  uint8_t scoped_key[32]{};
  const bool grant =
      grants_.activeGrant(tracker_id, key_epoch, scope, scoped_key);
  wipe(scoped_key, sizeof(scoped_key));
  return grant && scope.tracker_device_id == tracker_id &&
         scope.gateway_device_id == gateway_id &&
         scope.key_epoch == key_epoch &&
         grants_.verifiedForLocalGateway(gateway_id);
}

}  // namespace orun_tlp::kq05_ram_relay
#endif
