#include "kq05_backend_receipt.h"

#include <string.h>

#include "custody_ack_primitives.h"
#include "product_record_store.h"
#include "security_store.h"

namespace orun_tlp {
namespace {
constexpr char kKeyInfo[] = "ORUN-KQ05-BATCH-KEY-V1";
constexpr char kDomain[] = "ORUN-KQ05-BACKEND-DURABLE-V1";
// Python domain includes the final NUL, unlike the HKDF info label.
constexpr size_t kInfoSize = sizeof(kKeyInfo) - 1U + 8U + 4U;
constexpr size_t kMaxMacSize =
    sizeof(kDomain) + kq05_receipt::kMaxWireSize - kq05_receipt::kTagSize;
static_assert(kInfoSize <= custody_crypto::kMaxBoundedInfo,
              "KQ05 HKDF info exceeds CC310 bound");
static_assert(kMaxMacSize <= custody_crypto::kMaxBoundedData,
              "KQ05 HMAC input exceeds CC310 bound");
static_assert(kq05_receipt::kMaxWireSize == 72U, "KQ05 wire geometry");
uint32_t be32(const uint8_t* bytes) {
  return (uint32_t(bytes[0]) << 24U) |
         (uint32_t(bytes[1]) << 16U) |
         (uint32_t(bytes[2]) << 8U) | uint32_t(bytes[3]);
}
uint64_t be64(const uint8_t* bytes) {
  uint64_t v = 0U;
  for (size_t i = 0U; i < 8U; ++i) v = (v << 8U) | bytes[i];
  return v;
}
void wipe(void* p, size_t n) {
  volatile uint8_t* bytes = static_cast<volatile uint8_t*>(p);
  while (n-- != 0U) *bytes++ = 0U;
}
bool equal16(const uint8_t* a, const uint8_t* b) {
  volatile uint8_t diff = 0U;
  for (size_t i = 0U; i < kq05_receipt::kTagSize; ++i)
    diff = static_cast<uint8_t>(diff | (a[i] ^ b[i]));
  return diff == 0U;
}
}  // namespace

bool kq05_receipt::decode(const uint8_t* wire, size_t length,
                          Header& output) {
  if (wire == nullptr || length < kMinWireSize ||
      length > kMaxWireSize ||
      wire[0] != kVersion || wire[1] != kType || wire[2] != 0U ||
      wire[3] == 0U || wire[3] > kMaxRecords ||
      length != kHeaderSize + 4U * wire[3] + kTagSize) return false;
  Header candidate{};
  candidate.count = wire[3];
  candidate.device_id = be64(wire + 4U);
  candidate.incarnation = be64(wire + 12U);
  candidate.key_epoch = be32(wire + 20U);
  if (candidate.device_id == 0U || candidate.incarnation == 0U ||
      candidate.key_epoch == UINT32_MAX) return false;
  for (size_t i = 0U; i < candidate.count; ++i) {
    candidate.sequences[i] = be32(wire + kHeaderSize + i*4U);
    if (candidate.sequences[i] == 0U ||
        (i != 0U && candidate.sequences[i-1U] >= candidate.sequences[i]))
      return false;
  }
  output = candidate;
  return true;
}

Kq05BackendReceiptVerifier::Result Kq05BackendReceiptVerifier::verify(
    const uint8_t* wire, size_t length,
    const ProductRecordStore& records,
    AuthenticatedKq05BackendReceipt& output) const {
  kq05_receipt::Header decoded{};
  if (!kq05_receipt::decode(wire, length, decoded))
    return Result::kInvalid;

  if (!security_.ready_ || security_.state_ != SecurityState::kProvisioned ||
      security_.busy() || !records.mounted() || records.preparing() ||
      !records.store().ready() || records.store().faulted())
    return Result::kUnavailable;

  const uint64_t local_id = security_.device_identity_.legacyUint64();
  if (decoded.device_id != local_id ||
      decoded.key_epoch != security_.credential_.key_epoch ||
      decoded.incarnation != records.store().incarnation())
    return Result::kScopeMismatch;

  uint8_t salt[16]{};
  uint8_t root[32]{};
  uint8_t key[32]{};
  uint8_t info[kInfoSize]{};
  uint8_t mac_input[kMaxMacSize]{};
  uint8_t mac[32]{};
  memcpy(salt, security_.credential_.credential_id, sizeof(salt));
  memcpy(root, security_.credential_.k_root, sizeof(root));
  memcpy(info, kKeyInfo, sizeof(kKeyInfo)-1U);
  memcpy(info+sizeof(kKeyInfo)-1U, wire+4U, 8U);
  memcpy(info+sizeof(kKeyInfo)-1U+8U, wire+20U, 4U);

  const size_t prefix = length - kq05_receipt::kTagSize;
  memcpy(mac_input, kDomain, sizeof(kDomain));
  memcpy(mac_input+sizeof(kDomain), wire, prefix);
  const bool derived = custody_crypto::hkdfSha256Bounded(
      salt, root, info, sizeof(info), key);
  wipe(root, sizeof(root));
  wipe(salt, sizeof(salt));
  wipe(info, sizeof(info));
  const bool mac_ok = derived && custody_crypto::hmacSha256Bounded(
      key, mac_input, sizeof(kDomain)+prefix, mac);
  wipe(key, sizeof(key));
  wipe(mac_input, sizeof(mac_input));
  if (!mac_ok) {
    wipe(mac, sizeof(mac));
    return Result::kUnavailable;
  }
  const bool authorized = equal16(mac, wire+prefix);
  wipe(mac, sizeof(mac));
  if (!authorized) return Result::kBadMac;

  // Recheck scope after the CC310 operation before making a capability.
  if (!security_.ready_ || security_.state_ != SecurityState::kProvisioned ||
      security_.busy() || decoded.device_id !=
          security_.device_identity_.legacyUint64() ||
      decoded.key_epoch != security_.credential_.key_epoch ||
      decoded.incarnation != records.store().incarnation())
    return Result::kScopeMismatch;

  AuthenticatedKq05BackendReceipt verified{};
  verified.header_ = decoded;
  verified.authenticated_ = true;
  output = verified;
  return Result::kAuthenticated;
}

}  // namespace orun_tlp
