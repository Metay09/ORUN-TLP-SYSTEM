#pragma once

#include <stddef.h>
#include <stdint.h>

#include "history_receipt_admission.h"
#include "history_secure_crypto.h"
#include "history_store.h"

namespace orun_tlp {

// M4P5C transport-neutral composition seam for raw BACKEND_DURABLE
// HISTORY_SECURE frames.
//
// This owner remains transport-neutral even though normal production now wires
// RadioManager's bounded raw-frame handoff into it. RadioManager, NetworkService
// and relay policy are deliberately not dependencies of this class.
//
// Exact ordering:
//   raw frame
//    -> HistorySecureCrypto AEAD open into one opaque authenticated tuple
//    -> HistoryReceiptAdmissionCoordinator replay submission
//    -> SecurityStore accepted=true
//    -> M4P5A History delivery application
//
// The opaque AuthenticatedBackendDurableReceipt never escapes this class.
// Therefore a future transport caller cannot split or remix authenticated
// packet/plaintext/credential components between crypto and replay admission.
class HistoryReceiptReceiver {
 public:
  enum class SubmitResult : uint8_t {
    kStarted,
    kBusy,
    kUnavailable,
    kInvalidFrame,
    kAuthRejected,
    kCryptoEngineError,
    kInvariantFailure,
  };

  struct Diagnostics {
    uint32_t frames_started = 0;
    uint32_t busy_rejections = 0;
    uint32_t unavailable = 0;
    uint32_t invalid_frames = 0;
    uint32_t auth_rejections = 0;
    uint32_t crypto_engine_errors = 0;
    uint32_t invariant_failures = 0;
  };

  using ServiceResult = HistoryReceiptAdmissionCoordinator::ServiceResult;

  HistoryReceiptReceiver(HistoryStore& history,
                         HistorySecureCrypto& crypto,
                         HistoryReceiptAdmissionCoordinator& admission)
      : history_(history), crypto_(crypto), admission_(admission) {}

  // Accepts one complete untrusted frozen M4P4 HISTORY_SECURE frame.
  //
  // kStarted means the frame authenticated and M4P5B accepted ownership of
  // the exact replay decision. History application is still forbidden until
  // service() observes SecurityStore accepted=true.
  //
  // kBusy is fail-closed and retryable: the existing admitted receipt keeps
  // sole ownership of SecurityStore's single A2D replay-result channel.
  SubmitResult submitBackendDurableFrame(const uint8_t* frame,
                                         size_t frame_size);

  ServiceResult service() { return admission_.service(); }
  bool pending() const { return admission_.pending(); }
  const Diagnostics& diagnostics() const { return diagnostics_; }

 private:
  HistoryStore& history_;
  HistorySecureCrypto& crypto_;
  HistoryReceiptAdmissionCoordinator& admission_;
  Diagnostics diagnostics_{};
};

}  // namespace orun_tlp
