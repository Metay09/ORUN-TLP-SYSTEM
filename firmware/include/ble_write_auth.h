#pragma once

#include <stddef.h>
#include <stdint.h>

#include "security_store.h"

namespace orun_tlp {

// 4c-1b (owner decision 2026-11-04): a setting may be written over Bluetooth
// only by a phone signed in to the account. The account server gives each of
// its phones a per-Tracker write key derived from the Tracker's root
// credential (never K_root itself); the Tracker derives the same key here.
//
// Anti-replay: every write uses a fresh single-use challenge the Tracker
// hands out just before it. The phone MACs the exact request bytes together
// with that challenge; the Tracker MACs its result with the same challenge,
// so the phone only shows "applied" when the Tracker itself said so.
//
//   K_w  = HKDF-SHA256(salt = credential_id, ikm = K_root,
//                      info = kKeyInfoLabel || device_id BE64 || epoch BE32)
//   tag  = HMAC-SHA256(K_w, domain || device_id BE64 || epoch BE32 ||
//                      challenge || body)[0..16)
//
// backend/orun_credentials.py ble_write_key() and the app's bleWrite.ts use
// the same bytes; the host test pins a shared vector.
namespace ble_write_auth {
constexpr uint8_t kChallengeSize = 16;
constexpr uint8_t kTagSize = 16;
constexpr uint32_t kChallengeLifetimeMs = 30000;
constexpr char kKeyInfoLabel[] = "ORUN-BLE-WRITE-KEY-v1";
constexpr char kRequestDomain[] = "ORUN-BLE-WRITE-REQ-v1";
constexpr char kResultDomain[] = "ORUN-BLE-WRITE-RES-v1";
constexpr size_t kLabelSize = sizeof(kKeyInfoLabel) - 1U;  // 21, all three
static_assert(sizeof(kRequestDomain) - 1U == kLabelSize, "domain size");
static_assert(sizeof(kResultDomain) - 1U == kLabelSize, "domain size");
// Longest MACed body (a write result without its tag).
constexpr size_t kMaxBody = 32;
}  // namespace ble_write_auth

// Unpredictable challenge bytes while the SoftDevice owns the RNG.
class BleChallengeSource {
 public:
  virtual ~BleChallengeSource() = default;
  virtual bool generate(uint8_t (&out)[ble_write_auth::kChallengeSize]) = 0;
};

// Production: sd_rand_application_vector_get (SoftDevice RNG pool). Returns
// false while the pool is short; the phone simply asks again.
class NrfBleChallengeSource : public BleChallengeSource {
 public:
  bool generate(uint8_t (&out)[ble_write_auth::kChallengeSize]) override;
};

enum class BleChallengeResult : uint8_t {
  kIssued = 0,
  kNoCredential = 1,  // not provisioned / store not ready
  kNoEntropy = 2,     // try again
};

// Loop-owned. One outstanding challenge at a time (single BLE client).
class BleWriteAuthenticator {
 public:
  BleWriteAuthenticator(const SecurityStore& security,
                        BleChallengeSource& source)
      : security_(security), source_(source) {}

  // A new challenge replaces any earlier one. Public facts only out.
  BleChallengeResult issue(uint32_t now, uint64_t& device_id,
                           uint32_t& key_epoch,
                           uint8_t (&challenge)[ble_write_auth::kChallengeSize]);

  // Always consumes the outstanding challenge, whatever the outcome. True
  // only for an unexpired challenge, the current credential and a matching
  // tag; then exactly one signResult() may follow.
  bool verify(uint32_t now, const uint8_t* body, size_t body_len,
              const uint8_t* tag);

  // MACs the Tracker's answer to the write verify() accepted, then forgets
  // the challenge. False if nothing was verified (or the session ended).
  bool signResult(const uint8_t* body, size_t body_len,
                  uint8_t (&tag)[ble_write_auth::kTagSize]);

  // Session start/end: drop any challenge.
  void reset();

  bool challengeOutstanding() const { return state_ == State::kIssued; }
  bool resultOwed() const { return state_ == State::kVerified; }

 private:
  enum class State : uint8_t { kIdle, kIssued, kVerified };

  bool tagFor(const char* domain, const uint8_t* body, size_t body_len,
              uint8_t (&tag)[ble_write_auth::kTagSize]) const;
  bool credentialMatches() const;

  const SecurityStore& security_;
  BleChallengeSource& source_;
  State state_ = State::kIdle;
  uint32_t issued_at_ms_ = 0;
  uint64_t device_id_ = 0;
  uint32_t key_epoch_ = 0;
  uint8_t credential_id_[security_format::kCredentialIdSize]{};
  uint8_t challenge_[ble_write_auth::kChallengeSize]{};
};

}  // namespace orun_tlp
