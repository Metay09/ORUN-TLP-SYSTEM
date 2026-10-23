#include "gateway_custody_ack_signer.h"

#include <string.h>

#include "custody_ack_primitives.h"
#include "tlp_v2_product_secure.h"

namespace orun_tlp {
namespace {

constexpr char kAckMacDomain[] = "ORUN-TLP-V2-CUSTODY-ACK-MAC-v1";
constexpr size_t kMacInputSize =
    sizeof(kAckMacDomain) - 1U + custody_ack::kHeaderSize + 32U;
static_assert(kMacInputSize == 102U, "SF5B custody MAC format changed");
static_assert(custody_ack::kWireSize == 56U, "SF5B ACK wire changed");

void put32(uint32_t value, uint8_t* out) {
  out[0] = static_cast<uint8_t>(value >> 24U);
  out[1] = static_cast<uint8_t>(value >> 16U);
  out[2] = static_cast<uint8_t>(value >> 8U);
  out[3] = static_cast<uint8_t>(value);
}

void put64(uint64_t value, uint8_t* out) {
  for (unsigned index = 0U; index != 8U; ++index)
    out[index] = static_cast<uint8_t>(value >> (56U - 8U * index));
}

void wipe(void* pointer, size_t length) {
  volatile uint8_t* byte = static_cast<volatile uint8_t*>(pointer);
  while (length-- != 0U) *byte++ = 0U;
}

}  // namespace

bool GatewayCustodyAckSigner::signAfterDurableAcceptance(
    const uint8_t* exact_object, size_t object_len,
    const GatewayCustodyAckScope& scope,
    const uint8_t (&custody_ack_key)[32],
    uint8_t (&ack_out)[custody_ack::kWireSize]) {
  // Fail closed before touching caller output. Zero/invalid epochs and scopes
  // never create an ACK, even when a later runtime owner is defective.
  if (exact_object == nullptr ||
      (object_len != tlp::kEventFrameSize &&
       object_len != tlp::kPeriodicFrameSize) ||
      scope.tracker_device_id == 0U || scope.gateway_device_id == 0U ||
      scope.key_epoch == UINT32_MAX ||
      scope.custody_floor == UINT32_MAX ||
      scope.grant_generation == 0U ||
      scope.grant_generation == UINT32_MAX)
    return false;

  custody_ack::Header header{};
  header.relay_allowed = true;
  header.object_len = static_cast<uint8_t>(object_len);
  header.tracker_device_id = scope.tracker_device_id;
  header.gateway_device_id = scope.gateway_device_id;
  header.key_epoch = scope.key_epoch;
  header.custody_floor = scope.custody_floor;
  header.grant_generation = scope.grant_generation;
  if (!custody_ack::matchesProtectedShape(header, exact_object, object_len))
    return false;

  uint8_t digest[32]{};
  uint8_t wire[custody_ack::kWireSize]{};
  uint8_t mac_input[kMacInputSize]{};
  uint8_t mac[32]{};
  bool success = false;
  do {
    if (!custody_crypto::sha256(exact_object, object_len, digest)) break;

    wire[0] = custody_ack::kVersion;
    wire[1] = custody_ack::kType;
    wire[2] = custody_ack::kRelayAllowed;  // mandatory, including return via relay
    wire[3] = header.object_len;
    put64(scope.tracker_device_id, wire + 4U);
    put64(scope.gateway_device_id, wire + 12U);
    put32(scope.key_epoch, wire + 20U);
    put32(scope.custody_floor, wire + 24U);
    put32(scope.grant_generation, wire + 28U);
    memcpy(wire + 32U, digest, custody_ack::kObjectRefSize);

    memcpy(mac_input, kAckMacDomain, sizeof(kAckMacDomain) - 1U);
    memcpy(mac_input + sizeof(kAckMacDomain) - 1U,
           wire, custody_ack::kHeaderSize);
    memcpy(mac_input + sizeof(kAckMacDomain) - 1U + custody_ack::kHeaderSize,
           digest, sizeof(digest));
    if (!custody_crypto::hmacSha256(custody_ack_key, mac_input,
                                    sizeof(mac_input), mac)) break;
    memcpy(wire + custody_ack::kHeaderSize, mac,
           custody_ack::kTagSize);
    custody_ack::Header decoded{};
    if (!custody_ack::decodeHeader(wire, sizeof(wire), decoded) ||
        !decoded.relay_allowed) break;
    memcpy(ack_out, wire, sizeof(wire));
    success = true;
  } while (false);
  wipe(digest, sizeof(digest));
  wipe(mac, sizeof(mac));
  wipe(mac_input, sizeof(mac_input));
  wipe(wire, sizeof(wire));
  return success;
}

}  // namespace orun_tlp
