#pragma once

#include <stddef.h>
#include <stdint.h>

#include "history_delivery_coordinator.h"
#include "security_store.h"
#include "tlp_v2_history_secure.h"

namespace orun_tlp {

// SF3B transport-neutral owner for the security ordering between an already
// authenticated BACKEND_DURABLE History receipt and M4P5A delivery application.
//
// The caller may invoke submitAuthenticatedReceipt() ONLY with packet,
// plaintext and credential-id outputs produced by the SAME successful
// HistorySecureCrypto::openBackendDurableReceipt() call.
//
// This class then freezes that exact authenticated security lifetime into the
// SecurityStore replay submission:
//   authenticated credential_id + packet.key_epoch + packet.security_counter
//
// History delivery is forbidden until SecurityStore returns accepted=true.
// SecurityStore::poll() remains externally owned by the composition root; this
// coordinator never drives flash or security polling itself.
//
// While a receipt is pending, this object must be the sole consumer of
// SecurityStore::takeA2dReplayResult(). There is no production A2D dispatcher
// yet; before another protected A2D family is activated, result ownership must
// be serialized through one reviewed receive owner rather than allowing
// competing consumers to steal each other's replay decisions.
class HistoryReceiptAdmissionCoordinator {
 public:
  enum class SubmitResult : uint8_t {
    kStarted,
    kBusy,
    kInvalidAuthenticatedInput,
    kSecurityUnavailable,
  };

  enum class ServiceResult : uint8_t {
    kIdle,
    kWaitingReplay,
    kWaitingDelivery,
    kReplayRejected,
    kApplied,
    kDuplicateOnly,
    kDeliveryInvalidReceipt,
    kDeliveryUnknownIdentity,
    kDeliverySetFull,
    kDeliveryInvariantFailure,
  };

  struct Diagnostics {
    uint32_t submissions_started = 0;
    uint32_t busy_rejections = 0;
    uint32_t invalid_authenticated_inputs = 0;
    uint32_t security_unavailable = 0;
    uint32_t replay_admissions = 0;
    uint32_t replay_rejections = 0;
    uint32_t delivery_waits = 0;
    uint32_t delivery_applied = 0;
    uint32_t delivery_duplicates = 0;
    uint32_t delivery_rejections = 0;
    uint32_t delivery_invariant_failures = 0;
  };

  HistoryReceiptAdmissionCoordinator(
      SecurityStore& security_store,
      HistoryDeliveryCoordinator& delivery)
      : security_store_(security_store), delivery_(delivery) {}

  // Starts replay admission for one already-authenticated receipt.
  //
  // The method deliberately does NOT re-read SecurityStore's current
  // credential. The exact authenticated credential-id snapshot returned by
  // HistorySecureCrypto must be supplied by the caller and is submitted
  // together with the authenticated packet epoch/counter.
  //
  // A kStarted result means SecurityStore accepted ownership of this replay
  // decision. The receipt is copied into bounded RAM and no second receipt may
  // be submitted until service() reaches a terminal result.
  SubmitResult submitAuthenticatedReceipt(
      const tlp::HistorySecurePacket& packet,
      const tlp::BackendDurableReceiptPlaintext& receipt,
      const uint8_t (&authenticated_credential_id)
          [security_format::kCredentialIdSize]);

  // Consumes at most one SecurityStore replay result and, only after an
  // accepted result, attempts M4P5A delivery application.
  //
  // kWaitingReplay means SecurityStore has not published its replay result yet.
  // kWaitingDelivery means replay admission is already accepted but History is
  // temporarily unavailable/busy; the exact receipt remains pending in RAM and
  // service() may be retried without consuming another security counter.
  //
  // All other non-idle values are terminal and clear the pending receipt.
  ServiceResult service();

  bool pending() const { return state_ != State::kIdle; }
  const Diagnostics& diagnostics() const { return diagnostics_; }

 private:
  enum class State : uint8_t {
    kIdle,
    kAwaitReplay,
    kAwaitDelivery,
  };

  void clearPending();

  SecurityStore& security_store_;
  HistoryDeliveryCoordinator& delivery_;
  State state_ = State::kIdle;
  tlp::BackendDurableReceiptPlaintext pending_receipt_{};
  uint64_t pending_history_incarnation_ = 0;
  Diagnostics diagnostics_{};
};

}  // namespace orun_tlp
