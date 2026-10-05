#pragma once

#include <stddef.h>
#include <stdint.h>

#include "history_delivery_coordinator.h"
#include "history_secure_crypto.h"
#include "security_store.h"

namespace orun_tlp {

// SF3B transport-neutral owner for the security ordering between an already
// authenticated BACKEND_DURABLE History receipt and M4P5A delivery application.
//
// The caller may invoke submitAuthenticatedReceipt() only with the opaque
// AuthenticatedBackendDurableReceipt produced by ONE successful
// HistorySecureCrypto::openBackendDurableReceipt() call. Packet, plaintext and
// credential lifetime are therefore inseparable at this seam.
//
// This class freezes that exact authenticated security lifetime into the
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
  // credential. It consumes the exact opaque tuple produced by
  // HistorySecureCrypto and submits that authenticated epoch/counter/lifetime
  // unchanged.
  //
  // A kStarted result means SecurityStore accepted ownership of this replay
  // decision. The receipt is copied into bounded RAM and no second receipt may
  // be submitted until service() reaches a terminal result.
  SubmitResult submitAuthenticatedReceipt(
      const AuthenticatedBackendDurableReceipt& authenticated);

  // Consumes at most one SecurityStore replay result and, only after an
  // accepted result, attempts M4P5A delivery application.
  //
  // kWaitingReplay means SecurityStore has not published its replay result yet.
  // kWaitingDelivery means replay admission is already accepted but History is
  // temporarily unavailable/busy; the exact receipt remains pending in RAM and
  // service() may be retried without consuming another security counter.
  //
  // All other non-idle values are terminal and clear the pending receipt.
  // kDeliveryInvariantFailure may still mean M4P5A committed a shorter,
  // strict-validated RAM prefix. Callers must re-read acknowledgedThrough()
  // before deciding what remains outstanding; the watermark is never rolled
  // back and the consumed A2D counter must never be retried.
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
