#include "history_store_forward_runtime.h"

#include "monotonic_time.h"
#include "tlp_position_packet.h"
#include "tlp_v2_history_secure.h"

namespace orun_tlp {
namespace {

bool buildHistoryObservation(const HistoryStore::Record& record,
                             tlp::HistoryObservationPlaintext& observation) {
  tlp::PositionPacket position{};
  if (!tlp::deserializePositionPacket(
          record.packet, tlp::kPositionPacketSize, &position)) {
    return false;
  }

  observation = {};
  observation.history_record_identity = record.identity;
  observation.gnss_utc_epoch_seconds = position.gnss_utc_epoch_seconds;
  observation.latitude_e7 = position.latitude_e7;
  observation.longitude_e7 = position.longitude_e7;
  observation.altitude_mm = position.altitude_mm;
  observation.hdop_x100 = position.hdop_x100;
  observation.satellites = position.satellites;

  if ((position.flags & tlp::kPositionFlagValidFix) != 0U)
    observation.position_flags |= tlp::kHistoryPositionFlagValidFix;
  if ((position.flags & tlp::kPositionFlagValidUtcTime) != 0U)
    observation.position_flags |= tlp::kHistoryPositionFlagValidUtcTime;
  if ((position.flags & tlp::kPositionFlag3dFix) != 0U)
    observation.position_flags |= tlp::kHistoryPositionFlag3dFix;

  return true;
}

}  // namespace

HistoryStoreForwardRuntime::Event
HistoryStoreForwardRuntime::serviceAdmission() {
  if (!admission_.pending()) return Event::kNone;

  switch (admission_.service()) {
    case HistoryReceiptAdmissionCoordinator::ServiceResult::kIdle:
    case HistoryReceiptAdmissionCoordinator::ServiceResult::kWaitingReplay:
    case HistoryReceiptAdmissionCoordinator::ServiceResult::kWaitingDelivery:
      return Event::kNone;

    case HistoryReceiptAdmissionCoordinator::ServiceResult::kApplied:
      ++diagnostics_.receipt_applied;
      return Event::kReceiptApplied;

    case HistoryReceiptAdmissionCoordinator::ServiceResult::kDuplicateOnly:
      ++diagnostics_.receipt_duplicates;
      return Event::kReceiptDuplicate;

    case HistoryReceiptAdmissionCoordinator::ServiceResult::kReplayRejected:
      ++diagnostics_.receipt_replay_rejections;
      return Event::kReceiptReplayRejected;

    case HistoryReceiptAdmissionCoordinator::ServiceResult::
        kDeliveryInvalidReceipt:
    case HistoryReceiptAdmissionCoordinator::ServiceResult::
        kDeliveryUnknownIdentity:
    case HistoryReceiptAdmissionCoordinator::ServiceResult::kDeliverySetFull:
    case HistoryReceiptAdmissionCoordinator::ServiceResult::
        kDeliveryInvariantFailure:
      ++diagnostics_.receipt_delivery_rejections;
      return Event::kReceiptRejected;
  }

  ++diagnostics_.receipt_delivery_rejections;
  return Event::kReceiptRejected;
}

HistoryStoreForwardRuntime::Event
HistoryStoreForwardRuntime::serviceReceivedFrame() {
  // M4P5B owns the one SecurityStore replay-result channel. Do not consume a
  // second radio frame while that owner is still pending.
  if (admission_.pending()) return Event::kNone;

  HistorySecureRxFrame frame{};
  if (!radio_.takeHistorySecureFrame(&frame)) return Event::kNone;

  tlp::HistorySecurePacket structural{};
  if (tlp::deserializeHistorySecurePacket(
          frame.payload, frame.size, &structural) !=
      tlp::HistorySecureDecodeStatus::kOk) {
    ++diagnostics_.receipt_invalid_frames;
    return Event::kReceiptInvalidFrame;
  }

  // A device/gateway may hear protected observations from other trackers.
  // This first tracker-side runtime does not decrypt or acknowledge them.
  // Consuming the bounded radio mailbox keeps them from blocking a later
  // backend receipt; SF4 owns opaque gateway upload.
  if (structural.security_context ==
          tlp::kHistorySecurityContextDeviceD2a &&
      structural.app_family == tlp::kHistoryAppFamilyObservation) {
    ++diagnostics_.opaque_observations_received;
    return Event::kOpaqueObservationReceived;
  }

  if (structural.security_context !=
          tlp::kHistorySecurityContextBackendA2d ||
      structural.app_family !=
          tlp::kHistoryAppFamilyBackendDurableReceipt ||
      !history_.ready() || history_.incarnation() == 0U) {
    ++diagnostics_.receipt_invalid_frames;
    return Event::kReceiptInvalidFrame;
  }

  ++diagnostics_.receipt_frames;

  AuthenticatedBackendDurableReceipt authenticated{};
  const auto crypto_result = crypto_.openBackendDurableReceipt(
      frame.payload, frame.size, history_.incarnation(), authenticated);

  switch (crypto_result) {
    case HistorySecureCryptoResult::kOk:
      break;
    case HistorySecureCryptoResult::kAuthRejected:
      ++diagnostics_.receipt_auth_rejections;
      return Event::kReceiptAuthRejected;
    case HistorySecureCryptoResult::kUnavailable:
      // Backend may reissue the same durable fact with a fresh transport
      // counter. Do not retain untrusted raw bytes indefinitely in RAM.
      ++diagnostics_.receipt_invalid_frames;
      return Event::kReceiptInvalidFrame;
    case HistorySecureCryptoResult::kInvalidArgument:
    case HistorySecureCryptoResult::kEngineError:
      ++diagnostics_.receipt_invalid_frames;
      return Event::kReceiptInvalidFrame;
  }

  const auto submit = admission_.submitAuthenticatedReceipt(authenticated);
  switch (submit) {
    case HistoryReceiptAdmissionCoordinator::SubmitResult::kStarted:
      return Event::kNone;
    case HistoryReceiptAdmissionCoordinator::SubmitResult::kBusy:
    case HistoryReceiptAdmissionCoordinator::SubmitResult::
        kSecurityUnavailable:
    case HistoryReceiptAdmissionCoordinator::SubmitResult::
        kInvalidAuthenticatedInput:
      ++diagnostics_.receipt_invalid_frames;
      return Event::kReceiptInvalidFrame;
  }

  ++diagnostics_.receipt_invalid_frames;
  return Event::kReceiptInvalidFrame;
}

bool HistoryStoreForwardRuntime::scheduleAllowsProbe(uint32_t now_ms) {
  if (!probe_schedule_started_) {
    probe_schedule_started_ = true;
    next_probe_at_ms_ = now_ms + initial_probe_delay_ms_;
    return false;
  }
  return monotonic::reached(now_ms, next_probe_at_ms_);
}

void HistoryStoreForwardRuntime::armNextProbe(uint32_t now_ms) {
  next_probe_at_ms_ = now_ms + probe_interval_ms_;
}

HistoryStoreForwardRuntime::Event
HistoryStoreForwardRuntime::serviceReplay(
    uint32_t now_ms, bool higher_priority_pending) {
  if (higher_priority_pending || admission_.pending() ||
      !history_.ready() || history_.incarnation() == 0U ||
      !radio_.canSend() || !scheduleAllowsProbe(now_ms)) {
    return Event::kNone;
  }

  HistoryStore::Record record{};
  if (!history_.readNextRetained(history_.acknowledgedThrough(), record)) {
    return Event::kNone;
  }

  tlp::HistoryObservationPlaintext observation{};
  if (!buildHistoryObservation(record, observation)) {
    ++diagnostics_.replay_invalid_records;
    armNextProbe(now_ms);
    return Event::kReplaySendFailed;
  }

  tlp::HistorySecurePacket secure{};
  ++diagnostics_.replay_attempts;
  const auto protected_result = crypto_.protectNextObservation(
      history_.incarnation(),
      0U,  // First runtime activation is direct only; secure relay is not on.
      observation, secure);

  if (protected_result == HistorySecureCryptoResult::kUnavailable) {
    ++diagnostics_.replay_crypto_unavailable;
    next_probe_at_ms_ = now_ms + kUnavailableRetryMs;
    return Event::kNone;
  }
  if (protected_result != HistorySecureCryptoResult::kOk) {
    ++diagnostics_.replay_local_failures;
    armNextProbe(now_ms);
    return Event::kReplaySendFailed;
  }

  uint8_t frame[tlp::kHistoryObservationPacketSize]{};
  if (!tlp::serializeHistorySecurePacket(secure, frame, sizeof(frame))) {
    ++diagnostics_.replay_local_failures;
    armNextProbe(now_ms);
    return Event::kReplaySendFailed;
  }

  last_replay_identity_ = record.identity;
  last_replay_security_counter_ = secure.security_counter;

  // A counter is already consumed at this point. If the driver gate/send fails,
  // wait for the bounded next probe instead of burning counters in a tight loop.
  armNextProbe(now_ms);
  if (!radio_.sendHistorySecurePacket(frame, sizeof(frame))) {
    ++diagnostics_.replay_local_failures;
    return Event::kReplaySendFailed;
  }

  ++diagnostics_.replay_sent;
  return Event::kReplaySent;
}

HistoryStoreForwardRuntime::Event
HistoryStoreForwardRuntime::update(
    uint32_t now_ms, bool higher_priority_pending) {
  const Event admission_event = serviceAdmission();
  if (admission_event != Event::kNone) return admission_event;

  const Event receive_event = serviceReceivedFrame();
  if (receive_event != Event::kNone) return receive_event;

  // A newly authenticated receipt may have just started an async replay
  // reservation. Never originate backlog traffic until that owner settles.
  if (admission_.pending()) return Event::kNone;

  return serviceReplay(now_ms, higher_priority_pending);
}

}  // namespace orun_tlp
