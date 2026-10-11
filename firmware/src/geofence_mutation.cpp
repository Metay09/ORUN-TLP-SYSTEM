#include "geofence_mutation.h"

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

bool mayChangeGeofence(const ApplicationAccessContext& access) {
  return access.channel == ApplicationAccessChannel::kUsbLocal;
}

}  // namespace

ConfigMutationSubmitResult GeofenceMutationOwner::submitReplace(
    ApplicationRequester requester, uint32_t request_id,
    ApplicationAccessContext access, const GeofenceAreaSetView& area_set) {
  return submit(requester, request_id, access, GeofenceMutationKind::kReplace,
                &area_set);
}

ConfigMutationSubmitResult GeofenceMutationOwner::submitClear(
    ApplicationRequester requester, uint32_t request_id,
    ApplicationAccessContext access) {
  return submit(requester, request_id, access, GeofenceMutationKind::kClear,
                nullptr);
}

ConfigMutationSubmitResult GeofenceMutationOwner::submit(
    ApplicationRequester requester, uint32_t request_id,
    ApplicationAccessContext access, GeofenceMutationKind kind,
    const GeofenceAreaSetView* area_set) {
  if (!accessBelongsToRequester(requester, access))
    return ConfigMutationSubmitResult::kRejected;
  // Take the single slot before looking at the resource.
  if (state_ != State::kIdle) return ConfigMutationSubmitResult::kBusy;
  // The store is finishing other work: nothing evaluated, slot not held.
  if (store_.busy()) return ConfigMutationSubmitResult::kBusy;

  result_ = GeofenceMutationResult();
  result_.requester = requester;
  result_.request_id = request_id;
  result_.kind = kind;

  if (!mayChangeGeofence(access)) {
    complete(ConfigMutationOutcome::kAccessDenied);
    return ConfigMutationSubmitResult::kAccepted;
  }
  if (!store_.ready()) {
    complete(ConfigMutationOutcome::kUnavailable);
    return ConfigMutationSubmitResult::kAccepted;
  }
  if (store_.maintenanceResetRequired() ||
      store_.tokenState() != GeofenceTokenState::kValid ||
      store_.resourceState() == GeofenceResourceState::kUnavailable) {
    complete(ConfigMutationOutcome::kMaintenance);
    return ConfigMutationSubmitResult::kAccepted;
  }

  // The store validates the complete set (every M6C rule) and refuses the
  // whole replacement if any area is invalid; a refusal it counts as a
  // rejected candidate is INVALID, anything else is not a geometry verdict.
  const uint32_t rejected_before = store_.diagnostics().rejected_candidates;
  const bool admitted = kind == GeofenceMutationKind::kReplace
                            ? store_.requestReplace(*area_set)
                            : store_.requestClear();
  if (!admitted) {
    complete(store_.diagnostics().rejected_candidates != rejected_before
                 ? ConfigMutationOutcome::kInvalid
                 : ConfigMutationOutcome::kUnavailable);
    return ConfigMutationSubmitResult::kAccepted;
  }
  if (!store_.busy()) {
    // Already the active resource: nothing written, token unchanged.
    complete(ConfigMutationOutcome::kUnchanged);
    return ConfigMutationSubmitResult::kAccepted;
  }
  state_ = State::kSaving;
  return ConfigMutationSubmitResult::kAccepted;
}

bool GeofenceMutationOwner::poll() {
  if (state_ != State::kSaving) return false;
  bool success = false;
  if (!store_.takeMutationResult(success)) {
    if (store_.busy()) return false;
    complete(ConfigMutationOutcome::kOutcomeUnknown);
    return false;
  }
  // Not confirmed is not "definitely not written": the store reconciles.
  complete(success ? ConfigMutationOutcome::kApplied
                   : ConfigMutationOutcome::kOutcomeUnknown);
  return success;
}

bool GeofenceMutationOwner::takeResult(ApplicationRequester requester,
                                       GeofenceMutationResult& result) {
  if (state_ != State::kResultReady || result_.requester != requester)
    return false;
  result = result_;
  result_ = GeofenceMutationResult();
  state_ = State::kIdle;
  return true;
}

void GeofenceMutationOwner::complete(ConfigMutationOutcome outcome) {
  result_.outcome = outcome;
  result_.state = store_.resourceState();
  result_.area_count = store_.areaCount();
  result_.total_vertex_count = store_.totalVertexCount();
  result_.token_valid = store_.stateToken(result_.token);
  if (!result_.token_valid) result_.token = geofence_format::StateToken();
  state_ = State::kResultReady;
}

}  // namespace orun_tlp
