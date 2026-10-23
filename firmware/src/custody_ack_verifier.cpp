#include "custody_ack_verifier.h"

#include <string.h>

#include "custody_ack_primitives.h"
#include "security_store.h"

namespace orun_tlp {
namespace {

static const char kAckInfoLabel[] = "ORUN-TLP-V2-GW-CUSTODY-ACK-v1";
static const char kMacDomain[] = "ORUN-TLP-V2-CUSTODY-ACK-MAC-v1";
static constexpr size_t kInfoSize =
    sizeof(kAckInfoLabel) - 1U + 8U + 4U + 8U + 4U + 4U;
static constexpr size_t kMacInputSize =
    sizeof(kMacDomain) - 1U + custody_ack::kHeaderSize + 32U;
static_assert(kInfoSize == 57U, "SF5B custody HKDF scope changed");
static_assert(kMacInputSize == 102U, "SF5B custody MAC scope changed");

void secureZero(void* ptr, size_t count) {
  volatile uint8_t* p = static_cast<volatile uint8_t*>(ptr);
  while (count-- != 0U) *p++ = 0U;
}

bool constantTimeEqual(const uint8_t* left, const uint8_t* right,
                       size_t length) {
  volatile uint8_t diff = 0U;
  for (size_t i = 0U; i < length; ++i)
    diff = static_cast<uint8_t>(diff | (left[i] ^ right[i]));
  return diff == 0U;
}

void put32(uint32_t value, uint8_t* out) {
  out[0] = static_cast<uint8_t>(value >> 24U);
  out[1] = static_cast<uint8_t>(value >> 16U);
  out[2] = static_cast<uint8_t>(value >> 8U);
  out[3] = static_cast<uint8_t>(value);
}
void put64(uint64_t value, uint8_t* out) {
  for (unsigned i = 0U; i < 8U; ++i)
    out[i] = static_cast<uint8_t>(value >> (56U - 8U*i));
}

void buildInfo(const custody_ack::Header& header,
               uint8_t (&info)[kInfoSize]) {
  size_t n = 0U;
  memcpy(info, kAckInfoLabel, sizeof(kAckInfoLabel)-1U);
  n += sizeof(kAckInfoLabel)-1U;
  put64(header.tracker_device_id, info+n);
  n+=8U;
  put32(header.key_epoch, info+n);
  n+=4U;
  put64(header.gateway_device_id, info+n);
  n+=8U;
  put32(header.custody_floor, info+n);
  n+=4U;
  put32(header.grant_generation, info+n);
}

} // namespace

CustodyAckVerifyResult CustodyAckVerifier::verifyCandidates(
    const uint8_t* ack, size_t length,
    const observation_store_control::ExactObject* objects,
    uint16_t count, AuthenticatedCustodyAck& result,
    uint16_t& selected_index) {
  custody_ack::Header header{};
  if (!custody_ack::decodeHeader(ack,length,header))
    return CustodyAckVerifyResult::kInvalid;

  if (!security_.custodyAckScopeMatches(
          header.tracker_device_id, header.gateway_device_id,
          header.key_epoch, header.custody_floor,
          header.grant_generation))
    return CustodyAckVerifyResult::kNoDurableGrant;

  if (objects == nullptr || count == 0U ||
      count > observation_store_format::kMaxExactCustodyObjects)
    return CustodyAckVerifyResult::kNoUniqueObject;

  uint8_t selected_digest[32]{};
  uint16_t selected = UINT16_MAX;
  uint16_t matches = 0U;
  for (uint16_t i=0U; i<count; ++i) {
    const observation_store_control::ExactObject& object=objects[i];
    if (!object.identity.valid() ||
        (object.record_kind != observation_store_format::RecordKind::kPeriodic &&
         object.record_kind != observation_store_format::RecordKind::kEvent) ||
        !custody_ack::matchesProtectedShape(
            header, object.object, object.object_size))
      continue;

    uint8_t digest[32]{};
    if (!custody_crypto::sha256(object.object,object.object_size,digest)) {
      secureZero(digest,sizeof(digest));
      secureZero(selected_digest,sizeof(selected_digest));
      return CustodyAckVerifyResult::kCryptoFailure;
    }
    const bool hint_matches =
        constantTimeEqual(digest,ack+32U,custody_ack::kObjectRefSize);
    if (hint_matches) {
      ++matches;
      selected=i;
      memcpy(selected_digest,digest,sizeof(digest));
    }
    secureZero(digest,sizeof(digest));
    // Never use the 64-bit hint as sufficient identity, even when a forged
    // collision might produce a different full SHA256 and different HMAC.
    if (matches > 1U) {
      secureZero(selected_digest,sizeof(selected_digest));
      return CustodyAckVerifyResult::kNoUniqueObject;
    }
  }
  if (matches != 1U || selected == UINT16_MAX) {
    secureZero(selected_digest,sizeof(selected_digest));
    return CustodyAckVerifyResult::kNoUniqueObject;
  }

  // Only this friend of the live SecurityStore may touch K_root.
  // Wire fields are NOT trusted to choose a root or install an authority.
  uint8_t salt[16]{};
  uint8_t root[32]{};
  uint8_t key[32]{};
  uint8_t info[kInfoSize]{};
  uint8_t mac_input[kMacInputSize]{};
  uint8_t expected[32]{};
  memcpy(salt,security_.credential_.credential_id,sizeof(salt));
  memcpy(root,security_.credential_.k_root,sizeof(root));
  buildInfo(header,info);
  const bool derived=custody_crypto::hkdfSha256(
      salt,root,info,sizeof(info),key);
  secureZero(root,sizeof(root));
  secureZero(salt,sizeof(salt));
  secureZero(info,sizeof(info));

  bool calculated=false;
  if (derived) {
    memcpy(mac_input,kMacDomain,sizeof(kMacDomain)-1U);
    memcpy(mac_input+sizeof(kMacDomain)-1U,ack,
           custody_ack::kHeaderSize);
    memcpy(mac_input+sizeof(kMacDomain)-1U+custody_ack::kHeaderSize,
           selected_digest,sizeof(selected_digest));
    calculated=custody_crypto::hmacSha256(
        key,mac_input,sizeof(mac_input),expected);
  }
  secureZero(key,sizeof(key));
  secureZero(mac_input,sizeof(mac_input));
  if (!derived || !calculated) {
    secureZero(expected,sizeof(expected));
    secureZero(selected_digest,sizeof(selected_digest));
    return CustodyAckVerifyResult::kCryptoFailure;
  }

  const bool correct=constantTimeEqual(expected,ack+custody_ack::kHeaderSize,
                                       custody_ack::kTagSize);
  secureZero(expected,sizeof(expected));
  if (!correct ||
      // Detect any previously-unobserved state/policy change during
      // cryptographic work before publishing an authenticated capability.
      !security_.custodyAckScopeMatches(
          header.tracker_device_id,header.gateway_device_id,
          header.key_epoch,header.custody_floor,header.grant_generation)) {
    secureZero(selected_digest,sizeof(selected_digest));
    return correct ? CustodyAckVerifyResult::kNoDurableGrant :
                     CustodyAckVerifyResult::kBadMac;
  }

  AuthenticatedCustodyAck verified{};
  verified.identity_=objects[selected].identity;
  memcpy(verified.full_digest_,selected_digest,sizeof(selected_digest));
  verified.gateway_id_=header.gateway_device_id;
  verified.tracker_id_=header.tracker_device_id;
  verified.key_epoch_=header.key_epoch;
  verified.generation_=header.grant_generation;
  verified.floor_=header.custody_floor;
  verified.verified_=true;
  secureZero(selected_digest,sizeof(selected_digest));

  // Publish output only on success. Production store bridge additionally
  // rechecks byte-identical live retention BEFORE handing it to future owner.
  result=verified;
  selected_index=selected;
  return CustodyAckVerifyResult::kAuthenticated;
}

} // namespace orun_tlp
