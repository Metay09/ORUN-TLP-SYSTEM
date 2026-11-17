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
    const TrustedEdgeInstallation& installation)
    : grants_(grants), gateway_id_(gateway_id),
      installation_(installation) {}

bool SignedGrantCredentials::enrolledIdentity(
    char (&installation_id)[65]) const {
  memset(installation_id, 0, sizeof(installation_id));
  if (gateway_id_ == 0U || gateway_id_ == UINT64_MAX ||
      !grants_.verifiedForLocalGateway(gateway_id_))
    return false;
  char claimed[65]{};
  const bool present =
      installation_.readVerifiedInstallation(gateway_id_, claimed);
  const bool valid = present && validId(claimed, installation_id);
  wipe(claimed, sizeof(claimed));
  if (!valid) wipe(installation_id, sizeof(installation_id));
  return valid;
}

bool SignedGrantCredentials::verifiedForGateway(uint64_t gateway_id) const {
  if (gateway_id != gateway_id_) return false;
  char trusted_id[65]{};
  const bool enrolled = enrolledIdentity(trusted_id);
  wipe(trusted_id, sizeof(trusted_id));
  if (!enrolled) return false;
  uint8_t source_key[32]{};
  const bool allowed = grants_.edgeAcceptKey(source_key);
  wipe(source_key, sizeof(source_key));
  return allowed && grants_.verifiedForLocalGateway(gateway_id);
  // OGK1 v1: valid custody grants but NO Edge authority.
}

bool SignedGrantCredentials::readGatewayLink(
    uint64_t gateway_id, char (&edge_id)[65],
    uint8_t (&link_key)[32]) const {
  memset(edge_id, 0, sizeof(edge_id));
  wipe(link_key, sizeof(link_key));
  if (!verifiedForGateway(gateway_id)) return false;

  char trusted_id[65]{};
  char confirm_id[65]{};
  uint8_t master[32]{};
  uint8_t data[kMaxDeriveInput]{};
  uint8_t derived[32]{};
  bool success = false;
  do {
    if (!enrolledIdentity(trusted_id) ||
        !grants_.edgeAcceptKey(master)) break;
    const size_t id_size = strlen(trusted_id);
    memcpy(data, kDeriveDomain, sizeof(kDeriveDomain));
    put64(gateway_id, data + sizeof(kDeriveDomain));
    memcpy(data + sizeof(kDeriveDomain) + 8U, trusted_id, id_size);
    const size_t n = sizeof(kDeriveDomain) + 8U + id_size;
    if (!custody_crypto::hmacSha256Bounded(master, data, n, derived))
      break;
    // No stale installation ID or revoked signed authority may survive
    // the CC310 call. A changed/revoked enrollment invalidates the result.
    if (!grants_.verifiedForLocalGateway(gateway_id) ||
        !enrolledIdentity(confirm_id) ||
        strcmp(trusted_id, confirm_id) != 0) break;
    memcpy(link_key, derived, sizeof(derived));
    memcpy(edge_id, trusted_id, id_size);
    success = true;
  } while (false);
  wipe(trusted_id, sizeof(trusted_id));
  wipe(confirm_id, sizeof(confirm_id));
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
