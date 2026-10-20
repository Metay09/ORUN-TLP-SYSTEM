#pragma once

#include <stddef.h>
#include <stdint.h>

#include "custody_ack.h"
#include "gateway_custody_ack_signer.h"

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

  Result receive(const uint8_t* exact_object, size_t length);
  void service();
  // On denial or pending, output stays byte-identical. Revocation between
  // intake and ACK publication suppresses ACK, even if flash committed.
  bool takeAuthorizedAck(uint8_t (&output)[custody_ack::kWireSize]);
  bool pending() const { return pending_; }

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
