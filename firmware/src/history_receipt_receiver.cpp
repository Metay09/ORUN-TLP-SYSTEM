#include "history_receipt_receiver.h"

namespace orun_tlp {

HistoryReceiptReceiver::SubmitResult
HistoryReceiptReceiver::submitBackendDurableFrame(
    const uint8_t* frame, size_t frame_size) {
  // Preserve M4P5B's single replay-result owner before doing expensive crypto.
  // No second authenticated frame may overtake the pending receipt.
  if (admission_.pending()) {
    ++diagnostics_.busy_rejections;
    return SubmitResult::kBusy;
  }

  // History owns the incarnation expected by the authenticated receipt. Never
  // ask a transport/RF caller to supply or cache that identity boundary.
  if (!history_.ready() || history_.incarnation() == 0U) {
    ++diagnostics_.unavailable;
    return SubmitResult::kUnavailable;
  }

  AuthenticatedBackendDurableReceipt authenticated{};
  const auto crypto_result = crypto_.openBackendDurableReceipt(
      frame, frame_size, history_.incarnation(), authenticated);

  switch (crypto_result) {
    case HistorySecureCryptoResult::kOk:
      break;
    case HistorySecureCryptoResult::kUnavailable:
      ++diagnostics_.unavailable;
      return SubmitResult::kUnavailable;
    case HistorySecureCryptoResult::kInvalidArgument:
      ++diagnostics_.invalid_frames;
      return SubmitResult::kInvalidFrame;
    case HistorySecureCryptoResult::kAuthRejected:
      ++diagnostics_.auth_rejections;
      return SubmitResult::kAuthRejected;
    case HistorySecureCryptoResult::kEngineError:
      ++diagnostics_.crypto_engine_errors;
      return SubmitResult::kCryptoEngineError;
  }

  // Critical handoff: M4P5B receives the exact opaque object populated by the
  // successful AEAD open above. There is no packet/plaintext/credential
  // reconstruction or current-credential re-read in this composition seam.
  const auto admission_result =
      admission_.submitAuthenticatedReceipt(authenticated);

  switch (admission_result) {
    case HistoryReceiptAdmissionCoordinator::SubmitResult::kStarted:
      ++diagnostics_.frames_started;
      return SubmitResult::kStarted;
    case HistoryReceiptAdmissionCoordinator::SubmitResult::kBusy:
      ++diagnostics_.busy_rejections;
      return SubmitResult::kBusy;
    case HistoryReceiptAdmissionCoordinator::SubmitResult::
        kSecurityUnavailable:
      ++diagnostics_.unavailable;
      return SubmitResult::kUnavailable;
    case HistoryReceiptAdmissionCoordinator::SubmitResult::
        kInvalidAuthenticatedInput:
      // A successful HistorySecureCrypto opaque output should always satisfy
      // M4P5B's structural checks. Treat disagreement as an internal invariant
      // failure, not as a remotely-authenticated application fact.
      ++diagnostics_.invariant_failures;
      return SubmitResult::kInvariantFailure;
  }

  ++diagnostics_.invariant_failures;
  return SubmitResult::kInvariantFailure;
}

}  // namespace orun_tlp
