#pragma once

#include <stddef.h>
#include <stdint.h>

#include "custody_ack.h"
#include "gateway_custody_ack_signer.h"
#include "kq05_gateway_batch_custody_ack.h"
#include "product_custody_store.h"

namespace orun_tlp {

class GatewayLocalCustodyOwner;

// A TRUSTED provisioned Gateway service, never a radio packet or a Tracker
// SecurityStore. The provider owns/revokes its durable per-tracker grant and
// returns ONLY the 32-byte scoped K_custody_ack, NEVER K_root.
// The key must remain immutable for an unchanged complete scope. Any rotation
// or revocation must change/revoke its durable generation/floor/epoch.
class GatewayCustodyGrantProvider {
 public:
  virtual ~GatewayCustodyGrantProvider() = default;
  virtual bool activeGrant(uint64_t tracker_id, uint32_t key_epoch,
                           GatewayCustodyAckScope& scope,
                           uint8_t (&scoped_key)[32]) = 0;
};

// SF5K3S host-only composition seam; NOT automatically activated in main().
// Own on the application/loop task, never in SX1262 ISR or RX callback.
// Serialise this owner, grant mutations, and downstream RF TX admission.
// A structural RX frame is UNTRUSTED; this layer grants no origin AEAD proof.
class GatewayCustodyAuthorizedIngress final {
 public:
  enum class Result : uint8_t { kStarted, kAckReady, kBusy, kDenied };

  GatewayCustodyAuthorizedIngress(GatewayLocalCustodyOwner& custody,
                                  GatewayCustodyGrantProvider& grants,
                                  uint64_t local_gateway_id)
      : custody_(custody), grants_(grants), gateway_id_(local_gateway_id) {}
  GatewayCustodyAuthorizedIngress(const GatewayCustodyAuthorizedIngress&) = delete;
  GatewayCustodyAuthorizedIngress& operator=(
      const GatewayCustodyAuthorizedIngress&) = delete;

  // The experimental batch selection NEVER changes legacy 0x05 by default.
  // Untrusted RX cannot select batch_mode: only the trusted loop does.
  Result receive(const uint8_t* exact_object, size_t length,
                 bool batch_mode = false);
  void service();
  // On denial or pending, output stays byte-identical. Revocation between
  // intake and ACK publication suppresses ACK, even if flash committed.
  bool takeAuthorizedAck(uint8_t (&output)[custody_ack::kWireSize]);
  // Authoritative existing-grant check before a readback-proven Handle
  // leaves this ingress. Not a custody release or an on-air ACK.
  // Both outputs stay unchanged on failure.
  bool takeAuthorizedBatchHandle(ProductCustodyStore::Handle& handle,
                                 GatewayCustodyAckScope& scope);
  bool batchReady() const;

  // Trusted loop-only staging seam; NOT a radio sender or a new ingress
  // pipeline. Rechecks the CURRENT grant and the Gateway's exact durable
  // OCQ2 readback for 1..5 previously admitted handles, then verifies
  // that the grant is still active BEFORE publishing a scoped 0x0A.
  // A caller-supplied scope is merely a hint for grant lookup.
  // No output/length mutation on denial; no flash write or record release.
  bool prepareAuthorizedBatchAck(
      const ProductCustodyStore::Handle* handles, uint8_t count,
      const GatewayCustodyAckScope& requested,
      uint8_t (&wire)[kq05_gateway_batch_ack::kMaxSize],
      size_t& written);
  bool pending() const { return pending_; }
  // Identity bound at construction to the trusted Gateway custody owner.
  uint64_t localGatewayId() const { return gateway_id_; }

 private:
  void clearPending();
  static bool sameScope(const GatewayCustodyAckScope& a,
                        const GatewayCustodyAckScope& b);
  GatewayLocalCustodyOwner& custody_;
  GatewayCustodyGrantProvider& grants_;
  uint64_t gateway_id_ = 0U;
  bool pending_ = false;
  GatewayCustodyAckScope pending_scope_{};
};

}  // namespace orun_tlp
