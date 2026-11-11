#include "config_mutation.h"

#include "service_intent.h"

namespace orun_tlp {
namespace {

bool accessBelongsToRequester(ApplicationRequester requester,
                              const ApplicationAccessContext& access) {
  switch (requester) {
    case ApplicationRequester::kUsb:
      return access.channel == ApplicationAccessChannel::kUsbLocal;
    case ApplicationRequester::kBle:
      return access.channel == ApplicationAccessChannel::kBleOpen ||
             access.channel == ApplicationAccessChannel::kBleEncrypted ||
             access.channel == ApplicationAccessChannel::kBleAuthenticated;
    case ApplicationRequester::kLora:
      return access.channel == ApplicationAccessChannel::kLoraAuthenticated;
  }
  return false;
}

// Local USB is physical-access trust, the same level as the existing service
// commands. Over BLE only an account-authenticated write (4c-1b) may change
// configuration; an open or merely encrypted link may not.
bool mayChangeConfiguration(const ApplicationAccessContext& access) {
  return access.channel == ApplicationAccessChannel::kUsbLocal ||
         access.channel == ApplicationAccessChannel::kBleAuthenticated ||
         access.channel == ApplicationAccessChannel::kLoraAuthenticated;
}

// Applies one request's field(s) to the candidate. False: not admissible.
bool applyTo(config_format::Config& candidate, const ConfigMutationRequest& request) {
  switch (request.kind) {
    case ConfigMutationKind::kSetTrackingInterval:
      if (request.tracking_interval_seconds <
          config_mutation_policy::kMinTrackingIntervalSeconds)
        return false;
      candidate.tracking_interval_seconds = request.tracking_interval_seconds;
      return true;
    case ConfigMutationKind::kSetBleWindow:
      if (request.ble_window_seconds > config_format::kMaxBleWindowSeconds) return false;
      candidate.ble_window_seconds = static_cast<uint16_t>(request.ble_window_seconds);
      return true;
    case ConfigMutationKind::kSetServiceIntent:
      if (admitServiceIntent(request.service_mode, request.requested_services) !=
          ServiceIntentAdmission::kSupported)
        return false;
      candidate.service_mode = request.service_mode;
      candidate.requested_services = request.requested_services;
      return true;
  }
  return false;
}

}  // namespace

ConfigMutationSubmitResult ConfigMutationOwner::submit(
    const ConfigMutationRequest& request) {
  return submitBatch(&request, 1, nullptr);
}

ConfigMutationSubmitResult ConfigMutationOwner::submitBatch(
    const ConfigMutationRequest* items, size_t count,
    const config_format::StateToken* expected) {
  if (items == nullptr || count == 0 || count > kMaxBatch)
    return ConfigMutationSubmitResult::kRejected;
  const ConfigMutationRequest& first = items[0];
  for (size_t i = 0; i < count; ++i) {
    // One requester, one request id, one access context for the whole set.
    if (items[i].requester != first.requester || items[i].request_id != first.request_id ||
        items[i].access.channel != first.access.channel ||
        !accessBelongsToRequester(items[i].requester, items[i].access))
      return ConfigMutationSubmitResult::kRejected;
  }

  // Take the single mutation slot before evaluating anything about the
  // desired state, so two adapters can never both accept the same
  // precondition.
  if (state_ != State::kIdle) return ConfigMutationSubmitResult::kBusy;

  result_ = ConfigMutationResult();
  result_.requester = first.requester;
  result_.request_id = first.request_id;
  result_.kind = first.kind;

  if (!mayChangeConfiguration(first.access)) {
    complete(ConfigMutationOutcome::kAccessDenied);
    return ConfigMutationSubmitResult::kAccepted;
  }

  // Precondition (LoRa, owner decision 2026-11-11): the command was signed
  // for exactly this saved state. A replayed or overtaken command finds a
  // newer token and changes nothing.
  if (expected != nullptr) {
    config_format::StateToken current;
    if (!store_.stateToken(current)) {
      complete(ConfigMutationOutcome::kMaintenance);
      return ConfigMutationSubmitResult::kAccepted;
    }
    if (current.incarnation != expected->incarnation ||
        current.revision != expected->revision) {
      complete(ConfigMutationOutcome::kStale);
      return ConfigMutationSubmitResult::kAccepted;
    }
  }

  // Change the requested field(s) of the authoritative record; every other
  // field is carried over unchanged. All of them or none.
  config_format::Config candidate = store_.config();
  for (size_t i = 0; i < count; ++i) {
    if (!applyTo(candidate, items[i])) {
      complete(ConfigMutationOutcome::kInvalid);
      return ConfigMutationSubmitResult::kAccepted;
    }
  }

  switch (store_.admitSave(candidate)) {
    case ConfigSaveAdmission::kStarted:
      state_ = State::kSaving;
      return ConfigMutationSubmitResult::kAccepted;
    case ConfigSaveAdmission::kUnchanged:
      complete(ConfigMutationOutcome::kUnchanged);
      return ConfigMutationSubmitResult::kAccepted;
    case ConfigSaveAdmission::kBusy:
      // Nothing was evaluated against a stable state. Do not hold the slot
      // and do not return a token a caller could mistake for the result.
      result_ = ConfigMutationResult();
      return ConfigMutationSubmitResult::kBusy;
    case ConfigSaveAdmission::kInvalid:
      complete(ConfigMutationOutcome::kInvalid);
      return ConfigMutationSubmitResult::kAccepted;
    case ConfigSaveAdmission::kMaintenance:
      complete(ConfigMutationOutcome::kMaintenance);
      return ConfigMutationSubmitResult::kAccepted;
    case ConfigSaveAdmission::kUnavailable:
      complete(ConfigMutationOutcome::kUnavailable);
      return ConfigMutationSubmitResult::kAccepted;
  }
  complete(ConfigMutationOutcome::kUnavailable);
  return ConfigMutationSubmitResult::kAccepted;
}

bool ConfigMutationOwner::poll() {
  if (state_ != State::kSaving) return false;

  bool success = false;
  if (!store_.takeSaveResult(success)) {
    if (store_.busy()) return false;
    // The store finished but its result is gone (another consumer took it).
    // The change may or may not be durable: say so instead of waiting forever.
    complete(ConfigMutationOutcome::kOutcomeUnknown);
    return false;
  }

  // A store failure means "not confirmed", never "definitely not written".
  complete(success ? ConfigMutationOutcome::kApplied
                   : ConfigMutationOutcome::kOutcomeUnknown);
  return success;
}

bool ConfigMutationOwner::takeResult(ApplicationRequester requester,
                                     ConfigMutationResult& result) {
  if (state_ != State::kResultReady || result_.requester != requester)
    return false;
  result = result_;
  result_ = ConfigMutationResult();
  state_ = State::kIdle;
  return true;
}

void ConfigMutationOwner::complete(ConfigMutationOutcome outcome) {
  result_.outcome = outcome;
  result_.config = store_.config();
  result_.token_valid = store_.stateToken(result_.token);
  if (!result_.token_valid) result_.token = config_format::StateToken();
  state_ = State::kResultReady;
}

}  // namespace orun_tlp
