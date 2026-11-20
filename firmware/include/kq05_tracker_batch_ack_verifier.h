#pragma once

#include <stddef.h>
#include <stdint.h>

#include "kq05_gateway_batch_custody_ack.h"
#include "observation_store_format.h"

namespace orun_tlp {
class SecurityStore;
class ProductRecordStore;
class Kq05TrackerBatchAckVerifier;
class Kq05TrackerBatchReleaseOwner;

// An unforgeable-in-process capability of a verified FULL SHA256 per item,
// real SecurityStore V3 scope and MAC. This is NOT a released record.
// A caller cannot construct or alter the private verified identity set.
class AuthenticatedKq05TrackerBatchAck final {
 public:
  AuthenticatedKq05TrackerBatchAck() = default;
 private:
  friend class Kq05TrackerBatchAckVerifier;
  friend class Kq05TrackerBatchReleaseOwner;
#ifdef ORUN_KQ05_TRACKER_BATCH_VERIFY_HOST_TEST
  friend struct Kq05TrackerBatchVerifyHostPeer;
#endif
  observation_store_format::RecordIdentity identities_[
      kq05_gateway_batch_ack::kMaxObjects]{};
  uint8_t full_digests_[kq05_gateway_batch_ack::kMaxObjects][32]{};
  uint64_t tracker_id_ = 0U;
  uint64_t gateway_id_ = 0U;
  uint32_t key_epoch_ = UINT32_MAX;
  uint32_t floor_ = UINT32_MAX;
  uint32_t generation_ = 0U;
  uint8_t count_ = 0U;
  bool verified_ = false;
};

enum class Kq05TrackerBatchVerifyResult : uint8_t {
  kAuthenticated,
  kInvalid,
  kNoDurableGrant,
  kStoreUnavailable,
  kNoUniqueObject,
  kCryptoFailure,
  kBadMac,
};

// Strict read-only v2/type0x0A scope+MAC verifier, separate from single
// 0x05 Gateway custody and Backend durable 0x09 receipt. No release, erase
// or RF dispatch. Never accepts caller-provided "authenticated" tokens.
// Does not authorize a group from its 8-byte hints alone: resolves every
// reference to exactly one currently retained full 84/100-byte object.
class Kq05TrackerBatchAckVerifier final {
 public:
  explicit Kq05TrackerBatchAckVerifier(SecurityStore& security)
      : security_(security) {}

  Kq05TrackerBatchVerifyResult verifyFromStore(
      const uint8_t* wire, size_t length,
      const ProductRecordStore& store,
      AuthenticatedKq05TrackerBatchAck& result);

 private:
  SecurityStore& security_;
};

} // namespace orun_tlp
