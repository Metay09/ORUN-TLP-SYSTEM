#include "ble_write_auth.h"

#include <string.h>

#include "custody_ack_primitives.h"
#include "monotonic_time.h"

namespace orun_tlp {

namespace {

using ble_write_auth::kChallengeSize;
using ble_write_auth::kLabelSize;
using ble_write_auth::kTagSize;

void secureZero(void* p, size_t n) {
  volatile uint8_t* v = static_cast<volatile uint8_t*>(p);
  while (n-- != 0U) *v++ = 0U;
}

bool constantTimeEqual(const uint8_t* a, const uint8_t* b, size_t n) {
  uint8_t diff = 0U;
  for (size_t i = 0U; i < n; ++i) diff = static_cast<uint8_t>(diff | (a[i] ^ b[i]));
  return diff == 0U;
}

void putBe64(uint8_t* p, uint64_t v) {
  for (int i = 7; i >= 0; --i) { p[i] = static_cast<uint8_t>(v); v >>= 8; }
}
void putBe32(uint8_t* p, uint32_t v) {
  for (int i = 3; i >= 0; --i) { p[i] = static_cast<uint8_t>(v); v >>= 8; }
}

}  // namespace

BleChallengeResult BleWriteAuthenticator::issue(
    uint32_t now, uint64_t& device_id, uint32_t& key_epoch,
    uint8_t (&challenge)[kChallengeSize]) {
  // A fresh request always drops whatever was outstanding.
  reset();
  if (!security_.ready() || security_.state() != SecurityState::kProvisioned ||
      security_.credential_.device_identity == 0U)
    return BleChallengeResult::kNoCredential;
  uint8_t fresh[kChallengeSize]{};
  if (!source_.generate(fresh)) {
    secureZero(fresh, sizeof(fresh));
    return BleChallengeResult::kNoEntropy;
  }
  device_id_ = security_.credential_.device_identity;
  key_epoch_ = security_.credential_.key_epoch;
  memcpy(credential_id_, security_.credential_.credential_id,
         sizeof(credential_id_));
  memcpy(challenge_, fresh, sizeof(challenge_));
  secureZero(fresh, sizeof(fresh));
  issued_at_ms_ = now;
  state_ = State::kIssued;
  device_id = device_id_;
  key_epoch = key_epoch_;
  memcpy(challenge, challenge_, sizeof(challenge));
  return BleChallengeResult::kIssued;
}

bool BleWriteAuthenticator::credentialMatches() const {
  // The credential the challenge was issued for must still be the live one
  // (a re-provisioning in between makes the old challenge worthless).
  return security_.ready() && security_.state() == SecurityState::kProvisioned &&
         security_.credential_.device_identity == device_id_ &&
         security_.credential_.key_epoch == key_epoch_ &&
         memcmp(security_.credential_.credential_id, credential_id_,
                sizeof(credential_id_)) == 0;
}

bool BleWriteAuthenticator::tagFor(const char* domain, const uint8_t* body,
                                   size_t body_len,
                                   uint8_t (&tag)[kTagSize]) const {
  if (body == nullptr || body_len > ble_write_auth::kMaxBody) return false;
  uint8_t salt[security_format::kCredentialIdSize]{};
  uint8_t root[security_format::kKRootSize]{};
  uint8_t info[kLabelSize + 8U + 4U]{};
  uint8_t key[32]{};
  memcpy(salt, security_.credential_.credential_id, sizeof(salt));
  memcpy(root, security_.credential_.k_root, sizeof(root));
  memcpy(info, ble_write_auth::kKeyInfoLabel, kLabelSize);
  putBe64(info + kLabelSize, device_id_);
  putBe32(info + kLabelSize + 8U, key_epoch_);
  const bool derived =
      custody_crypto::hkdfSha256Bounded(salt, root, info, sizeof(info), key);
  secureZero(root, sizeof(root));
  secureZero(salt, sizeof(salt));

  uint8_t input[kLabelSize + 8U + 4U + kChallengeSize + ble_write_auth::kMaxBody]{};
  size_t at = 0U;
  memcpy(input, domain, kLabelSize);
  at += kLabelSize;
  putBe64(input + at, device_id_);
  at += 8U;
  putBe32(input + at, key_epoch_);
  at += 4U;
  memcpy(input + at, challenge_, kChallengeSize);
  at += kChallengeSize;
  memcpy(input + at, body, body_len);
  at += body_len;

  uint8_t mac[32]{};
  const bool calculated =
      derived && custody_crypto::hmacSha256Bounded(key, input, at, mac);
  secureZero(key, sizeof(key));
  secureZero(input, sizeof(input));
  if (calculated) memcpy(tag, mac, kTagSize);
  secureZero(mac, sizeof(mac));
  return calculated;
}

bool BleWriteAuthenticator::verify(uint32_t now, const uint8_t* body,
                                   size_t body_len, const uint8_t* tag) {
  if (state_ != State::kIssued) {
    reset();
    return false;
  }
  const bool fresh = !monotonic::elapsed(now, issued_at_ms_,
                                         ble_write_auth::kChallengeLifetimeMs);
  uint8_t expected[kTagSize]{};
  const bool ok = fresh && tag != nullptr && credentialMatches() &&
                  tagFor(ble_write_auth::kRequestDomain, body, body_len,
                         expected) &&
                  constantTimeEqual(expected, tag, kTagSize);
  secureZero(expected, sizeof(expected));
  if (!ok) {
    reset();
    return false;
  }
  state_ = State::kVerified;  // the challenge now only signs the answer
  return true;
}

bool BleWriteAuthenticator::signResult(const uint8_t* body, size_t body_len,
                                       uint8_t (&tag)[kTagSize]) {
  const bool ok = state_ == State::kVerified && credentialMatches() &&
                  tagFor(ble_write_auth::kResultDomain, body, body_len, tag);
  reset();
  return ok;
}

void BleWriteAuthenticator::reset() {
  state_ = State::kIdle;
  issued_at_ms_ = 0U;
  device_id_ = 0U;
  key_epoch_ = 0U;
  secureZero(credential_id_, sizeof(credential_id_));
  secureZero(challenge_, sizeof(challenge_));
}

}  // namespace orun_tlp
