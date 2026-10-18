#pragma once

#include <stddef.h>
#include <stdint.h>

#include "custody_ack.h"
#include "observation_store_control.h"

namespace orun_tlp {

class SecurityStore;
class ProductRecordStore;
class CustodyAckVerifier;
class CustodyAckReleaseOwner;

// This capability is created only after one full SHA256/HKDF/HMAC
// authentication against the real SecurityStore v3 custody grant.
// It is NOT a durable release result. Only a future reviewed release
// owner may access its identity; no radio callback gets that privilege.
class AuthenticatedCustodyAck {
 public:
  AuthenticatedCustodyAck() = default;

 private:
  friend class CustodyAckVerifier;
  friend class CustodyAckReleaseOwner;
#ifdef ORUN_SF5K3L_HOST_TEST
  friend struct CustodyAckVerifierHostPeer;
#endif
  observation_store_format::RecordIdentity identity_{};
  uint8_t full_digest_[32]{};
  uint64_t gateway_id_ = 0U;
  uint32_t generation_ = 0U;
  uint32_t floor_ = 0U;
  // Default construction is deliberately NOT a valid security capability.
  // A future release owner must check this private verified bit and rebind
  // the exact logical identity to current retained flash before release.
  bool verified_ = false;
};

enum class CustodyAckVerifyResult : uint8_t {
  kAuthenticated,
  kInvalid,
  kNoDurableGrant,
  kStoreUnavailable,
  kNoUniqueObject,
  kCryptoFailure,
  kBadMac,
};

// No public K_root accessor and no capability granted by a mere ACK header.
// Real path snapshots only retained, durably cached exact objects from
// ProductRecordStore, then requires byte-identical store binding.
// This class performs NO record release and NO ACK generation.
class CustodyAckVerifier {
 public:
  explicit CustodyAckVerifier(SecurityStore& security) : security_(security) {}

  CustodyAckVerifyResult verifyFromStore(
      const uint8_t* ack, size_t length,
      const ProductRecordStore& product_store,
      AuthenticatedCustodyAck& authenticated);

 private:
#ifdef ORUN_SF5K3L_HOST_TEST
  friend struct CustodyAckVerifierHostPeer;
#endif
  CustodyAckVerifyResult verifyCandidates(
      const uint8_t* ack, size_t length,
      const observation_store_control::ExactObject* candidates,
      uint16_t count, AuthenticatedCustodyAck& authenticated,
      uint16_t& selected_index);

  SecurityStore& security_;
};

}  // namespace orun_tlp
