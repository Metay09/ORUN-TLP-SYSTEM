#include "ble_write_service.h"

#include <string.h>

namespace orun_tlp {

namespace {

namespace bat = ble_app_transport;

void putLe16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}
void putLe32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) { p[i] = static_cast<uint8_t>(v); v >>= 8; }
}
uint32_t getLe32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
         (uint32_t(p[3]) << 24);
}

}  // namespace

bool BleWriteService::challenge(uint32_t now, uint8_t* payload,
                                uint16_t& length,
                                bat::ErrorCode& error) {
  length = 0;
  if (!writable_) {
    auth_.reset();
    error = bat::ErrorCode::kAccessDenied;
    return false;
  }
  uint64_t device_id = 0;
  uint32_t epoch = 0;
  uint8_t challenge[ble_write_auth::kChallengeSize]{};
  switch (auth_.issue(now, device_id, epoch, challenge)) {
    case BleChallengeResult::kIssued:
      break;
    case BleChallengeResult::kNoEntropy:
      error = bat::ErrorCode::kBusy;
      return false;
    case BleChallengeResult::kNoCredential:
    default:
      error = bat::ErrorCode::kAccessDenied;
      return false;
  }
  payload[0] = bat::kApplicationStatusOk;
  for (int i = 0; i < 8; ++i)
    payload[1 + i] = static_cast<uint8_t>(device_id >> (56 - 8 * i));
  putLe32(payload + 9, epoch);
  memcpy(payload + 13, challenge, sizeof(challenge));
  length = bat::kWriteChallengePayloadSize;
  return true;
}

BleWriteAdmission BleWriteService::submitWrite(uint32_t now,
                                               uint32_t request_id,
                                               const uint8_t* payload,
                                               uint16_t length) {
  if (!writable_) {
    auth_.reset();
    return BleWriteAdmission::kUnsupported;
  }
  if (payload == nullptr || length != bat::kWriteRequestPayloadSize ||
      owed_ || immediate_) {
    auth_.reset();
    return owed_ || immediate_ ? BleWriteAdmission::kBusy
                               : BleWriteAdmission::kDenied;
  }
  if (!auth_.verify(now, payload, bat::kWriteRequestBodySize,
                    payload + bat::kWriteRequestBodySize))
    return BleWriteAdmission::kDenied;

  // Authenticated from here on: every answer is signed, refusals included.
  const uint8_t op = payload[0];
  const uint32_t value = getLe32(payload + 1);
  ConfigMutationKind kind = ConfigMutationKind::kSetTrackingInterval;
  uint32_t interval = 0, ble_window = 0;
  uint8_t mode = 0, services = 0;
  switch (op) {
    case bat::kWriteOpInterval:
      interval = value;
      break;
    case bat::kWriteOpBleWindow:
      kind = ConfigMutationKind::kSetBleWindow;
      ble_window = value;
      break;
    case bat::kWriteOpServices: {
      kind = ConfigMutationKind::kSetServiceIntent;
      mode = static_cast<uint8_t>(value);
      services = static_cast<uint8_t>(value >> 8);
      // Y2a: a Tracker keeps its role; an intent for the other role is
      // refused, never saved (same rule as the USB writer).
      const StorageRole asked = storageRoleForIntent(mode, services);
      if ((value >> 16) != 0U ||
          (asked != StorageRole::kUnassigned && asked != role_)) {
        prepareImmediate(request_id, op, ConfigMutationOutcome::kInvalid);
        return BleWriteAdmission::kPending;
      }
      break;
    }
    default:
      prepareImmediate(request_id, op, ConfigMutationOutcome::kInvalid);
      return BleWriteAdmission::kPending;
  }

  const ConfigMutationRequest request(
      ApplicationRequester::kBle, request_id,
      ApplicationAccessContext(ApplicationAccessChannel::kBleAuthenticated),
      kind, interval, mode, services, ble_window);
  switch (mutations_.submit(request)) {
    case ConfigMutationSubmitResult::kAccepted:
      owed_ = true;
      owed_request_id_ = request_id;
      owed_op_ = op;
      return BleWriteAdmission::kPending;
    case ConfigMutationSubmitResult::kBusy:
      auth_.reset();
      return BleWriteAdmission::kBusy;
    case ConfigMutationSubmitResult::kRejected:
    default:
      auth_.reset();
      return BleWriteAdmission::kUnsupported;
  }
}

void BleWriteService::prepareImmediate(uint32_t request_id, uint8_t op,
                                       ConfigMutationOutcome outcome) {
  immediate_ = true;
  immediate_op_ = op;
  immediate_result_ = ConfigMutationResult();
  immediate_result_.requester = ApplicationRequester::kBle;
  immediate_result_.request_id = request_id;
  immediate_result_.outcome = outcome;
  immediate_result_.config = store_.config();
  immediate_result_.token_valid = store_.stateToken(immediate_result_.token);
  if (!immediate_result_.token_valid)
    immediate_result_.token = config_format::StateToken();
}

void BleWriteService::sessionReset() {
  // The challenge and any immediate answer die with the session. A write the
  // owner already holds still finishes; takeAnswer() swallows its result.
  auth_.reset();
  immediate_ = false;
}

bool BleWriteService::build(
    const ConfigMutationResult& result, uint8_t op,
    uint8_t (&payload)[bat::kWriteResultPayloadSize]) {
  memset(payload, 0, sizeof(payload));
  payload[0] = static_cast<uint8_t>(result.outcome);
  payload[1] = op;
  putLe32(payload + 2, result.config.tracking_interval_seconds);
  putLe16(payload + 6, result.config.ble_window_seconds);
  payload[8] = result.config.service_mode;
  payload[9] = result.config.requested_services;
  putLe32(payload + 10, result.token_valid ? result.token.revision : 0U);
  uint8_t tag[ble_write_auth::kTagSize]{};
  if (!auth_.signResult(payload, bat::kWriteResultBodySize, tag)) return false;
  memcpy(payload + bat::kWriteResultBodySize, tag, sizeof(tag));
  return true;
}

BleWriteService::Answer BleWriteService::takeAnswer(
    uint32_t& request_id, uint8_t (&payload)[bat::kWriteResultPayloadSize],
    ConfigMutationResult& result) {
  if (immediate_) {
    immediate_ = false;
    result = immediate_result_;
    request_id = result.request_id;
    return build(result, immediate_op_, payload) ? Answer::kReady
                                                 : Answer::kDropped;
  }
  ConfigMutationResult taken;
  if (!mutations_.takeResult(ApplicationRequester::kBle, taken))
    return Answer::kNone;
  result = taken;
  request_id = taken.request_id;
  if (!owed_ || taken.request_id != owed_request_id_) {
    owed_ = false;
    auth_.reset();
    return Answer::kDropped;
  }
  owed_ = false;
  return build(taken, owed_op_, payload) ? Answer::kReady : Answer::kDropped;
}

}  // namespace orun_tlp
