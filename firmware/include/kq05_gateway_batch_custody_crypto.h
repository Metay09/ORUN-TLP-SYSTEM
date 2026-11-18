#pragma once
#include <stddef.h>
#include <stdint.h>

#include "gateway_custody_ack_signer.h"
#include "kq05_gateway_batch_custody_ack.h"

namespace orun_tlp {

// An exact protected PRODUCT_SECURE object; this view conveys NO durability
// or authenticated enrollment by itself. Never derive a custody ACK just
// because an RX frame is shaped correctly.
struct Kq05BatchExactView {
  const uint8_t* bytes = nullptr;
  size_t size = 0U;
};

// Internal crypto contract. Sign and verify are deliberately NOT callable
// from radio callbacks or arbitrary product ingress. The future privileged
// owners must source objects from actual durable Gateway readback and
// Tracker retained observation state, and source key/scope from CURRENT
// immutable grants. This PC slice creates neither owner.
// Scoped custody key is the existing 32-byte Gateway K_custody_ack;
// BACKEND receipt keys MUST NOT be used.
class Kq05GatewayBatchCustodyCrypto final {
 public:
  Kq05GatewayBatchCustodyCrypto() = delete;
 private:
  friend class Kq05GatewayDurableBatchOwner;
  friend class Kq05TrackerBatchReleaseOwner;
#ifdef ORUN_KQ05_BATCH_HMAC_HOST_TEST
  friend struct Kq05BatchCryptoHostPeer;
#endif

  // NO output modification on failure. On success, 'written' is exactly
  // 56/64/72/80/88. The caller must PROVE all N objects durably stored.
  static bool signAfterDurableReadback(
      const GatewayCustodyAckScope& scope,
      const uint8_t (&scoped_key)[32],
      const Kq05BatchExactView* exact_objects,
      uint8_t count,
      uint8_t (&out)[kq05_gateway_batch_ack::kMaxSize],
      size_t& written);

  // Does NOT issue an authenticated release capability; it only compares
  // the exact full-digest HMAC against the supplied set of retained bytes.
  // The future Tracker owner must re-check live grant scope and durable
  // retention before starting each selective release marker.
  static bool verifyAgainstExactRetained(
      const uint8_t* wire, size_t wire_size,
      const GatewayCustodyAckScope& trusted_scope,
      const uint8_t (&scoped_key)[32],
      const Kq05BatchExactView* retained_objects,
      uint8_t count);
};

} // namespace orun_tlp
