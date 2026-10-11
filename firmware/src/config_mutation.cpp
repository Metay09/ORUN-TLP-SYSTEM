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
             access.channel == ApplicationAccessChannel::kBleEncrypted;
  }
  return false;
}

// Local USB is physical-access trust, the same level as the existing service
// commands. BLE needs reviewed authentication and authorization first.
bool mayChangeConfiguration(const ApplicationAccessContext& access) {
  return access.channel == ApplicationAccessChannel::kUsbLocal;
}

}  // namespace

ConfigMutationSubmitResult ConfigMutationOwner::submit(
    const ConfigMutationRequest& request) {
  if (!accessBelongsToRequester(request.requester, request.access))
    return ConfigMutationSubmitResult::kRejected;

  // Take the single mutation slot before evaluating anything about the
  // desired state, so two adapters can never both accept the same
  // precondition.
  if (state_ != State::kIdle) return ConfigMutationSubmitResult::kBusy;

  result_ = ConfigMutationResult();
  result_.requester = request.requester;
  result_.request_id = request.request_id;
  result_.kind = request.kind;

  if (!mayChangeConfiguration(request.access)) {
    complete(ConfigMutationOutcome::kAccessDenied);
    return ConfigMutationSubmitResult::kAccepted;
  }

  // Change the requested field(s) of the authoritative record; every other
  // field is carried over unchanged.
  config_format::Config candidate = store_.config();
  switch (request.kind) {
    case ConfigMutationKind::kSetTrackingInterval:
      if (request.tracking_interval_seconds <
          config_mutation_policy::kMinTrackingIntervalSeconds) {
        complete(ConfigMutationOutcome::kInvalid);
        return ConfigMutationSubmitResult::kAccepted;
      }
      candidate.tracking_interval_seconds = request.tracking_interval_seconds;
      break;
    case ConfigMutationKind::kSetServiceIntent:
      if (admitServiceIntent(request.service_mode,
                             request.requested_services) !=
          ServiceIntentAdmission::kSupported) {
        complete(ConfigMutationOutcome::kInvalid);
        return ConfigMutationSubmitResult::kAccepted;
      }
      candidate.service_mode = request.service_mode;
      candidate.requested_services = request.requested_services;
      break;
    default:
      complete(ConfigMutationOutcome::kInvalid);
      return ConfigMutationSubmitResult::kAccepted;
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
