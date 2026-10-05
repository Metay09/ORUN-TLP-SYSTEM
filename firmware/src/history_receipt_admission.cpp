#include "history_receipt_admission.h"

#include <string.h>

namespace orun_tlp {

void HistoryReceiptAdmissionCoordinator::clearPending() {
  pending_receipt_ = {};
  pending_history_incarnation_ = 0;
  state_ = State::kIdle;
}

HistoryReceiptAdmissionCoordinator::SubmitResult
HistoryReceiptAdmissionCoordinator::submitAuthenticatedReceipt(
    const tlp::HistorySecurePacket& packet,
    const tlp::BackendDurableReceiptPlaintext& receipt,
    const uint8_t (&authenticated_credential_id)
        [security_format::kCredentialIdSize]) {
  if (state_ != State::kIdle) {
    ++diagnostics_.busy_rejections;
    return SubmitResult::kBusy;
  }

  const size_t receipt_size =
      tlp::backendDurableReceiptPlaintextSize(receipt.count);
  uint8_t encoded_receipt[tlp::kHistoryReceiptMaxPlaintextSize]{};

  // These checks do not replace AEAD authentication. They only make misuse of
  // this post-authentication seam fail closed if unrelated packet/plaintext
  // objects are accidentally paired by a future runtime caller.
  if (!tlp::validateHistorySecurePacket(packet) ||
      packet.security_context != tlp::kHistorySecurityContextBackendA2d ||
      packet.app_family !=
          tlp::kHistoryAppFamilyBackendDurableReceipt ||
      receipt_size == 0 ||
      packet.ciphertext_len != receipt_size ||
      !tlp::serializeBackendDurableReceiptPlaintext(
          receipt, encoded_receipt, receipt_size)) {
    ++diagnostics_.invalid_authenticated_inputs;
    return SubmitResult::kInvalidAuthenticatedInput;
  }

  // Critical ordering boundary: submit the EXACT lifetime authenticated by
  // HistorySecureCrypto. Never re-read currentCredentialId() here.
  if (!security_store_.submitAuthenticatedA2dCounter(
          authenticated_credential_id,
          packet.key_epoch,
          packet.security_counter)) {
    ++diagnostics_.security_unavailable;
    return SubmitResult::kSecurityUnavailable;
  }

  pending_receipt_ = receipt;
  pending_history_incarnation_ = packet.history_incarnation;
  state_ = State::kAwaitReplay;
  ++diagnostics_.submissions_started;
  return SubmitResult::kStarted;
}

HistoryReceiptAdmissionCoordinator::ServiceResult
HistoryReceiptAdmissionCoordinator::service() {
  if (state_ == State::kIdle) return ServiceResult::kIdle;

  if (state_ == State::kAwaitReplay) {
    bool accepted = false;
    if (!security_store_.takeA2dReplayResult(accepted))
      return ServiceResult::kWaitingReplay;

    if (!accepted) {
      ++diagnostics_.replay_rejections;
      clearPending();
      return ServiceResult::kReplayRejected;
    }

    ++diagnostics_.replay_admissions;
    state_ = State::kAwaitDelivery;
  }

  const auto delivery_result =
      delivery_.applyReplayAcceptedBackendDurableReceipt(
          pending_receipt_, pending_history_incarnation_);

  switch (delivery_result) {
    case HistoryDeliveryCoordinator::ApplyResult::kApplied:
      ++diagnostics_.delivery_applied;
      clearPending();
      return ServiceResult::kApplied;

    case HistoryDeliveryCoordinator::ApplyResult::kDuplicateOnly:
      ++diagnostics_.delivery_duplicates;
      clearPending();
      return ServiceResult::kDuplicateOnly;

    case HistoryDeliveryCoordinator::ApplyResult::kUnavailable:
      ++diagnostics_.delivery_waits;
      return ServiceResult::kWaitingDelivery;

    case HistoryDeliveryCoordinator::ApplyResult::kInvalidReceipt:
      ++diagnostics_.delivery_rejections;
      clearPending();
      return ServiceResult::kDeliveryInvalidReceipt;

    case HistoryDeliveryCoordinator::ApplyResult::kUnknownIdentity:
      ++diagnostics_.delivery_rejections;
      clearPending();
      return ServiceResult::kDeliveryUnknownIdentity;

    case HistoryDeliveryCoordinator::ApplyResult::kSelectiveSetFull:
      ++diagnostics_.delivery_rejections;
      clearPending();
      return ServiceResult::kDeliverySetFull;

    case HistoryDeliveryCoordinator::ApplyResult::kInvariantFailure:
      ++diagnostics_.delivery_invariant_failures;
      clearPending();
      return ServiceResult::kDeliveryInvariantFailure;
  }

  ++diagnostics_.delivery_invariant_failures;
  clearPending();
  return ServiceResult::kDeliveryInvariantFailure;
}

}  // namespace orun_tlp
