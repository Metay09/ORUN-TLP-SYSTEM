#include "application_request.h"

#include "config_store.h"

namespace orun_tlp {

bool ApplicationRequestService::requesterSupported(
    ApplicationRequester requester) {
  return requester == ApplicationRequester::kUsb ||
         requester == ApplicationRequester::kBle;
}

bool ApplicationRequestService::requestKindSupported(
    ApplicationRequestKind kind) {
  switch (kind) {
    case ApplicationRequestKind::kGetConfig:
    case ApplicationRequestKind::kGetDeviceStatus:
    case ApplicationRequestKind::kGetTrackingStatus:
    case ApplicationRequestKind::kGetGeofenceStatus:
    case ApplicationRequestKind::kGetStorageStatus:
      return true;
  }
  return false;
}

bool ApplicationRequestService::accessAllowed(
    ApplicationRequestKind kind, const ApplicationAccessContext& access) {
  if (!requestKindSupported(kind)) return false;

  // M7P7H exposes only bounded read-only local diagnostics. Final
  // authentication/authorization is intentionally later, but admission lives
  // here now so USB/BLE/future LoRa do not grow separate operation allow-lists.
  switch (access.channel) {
    case ApplicationAccessChannel::kUsbLocal:
    case ApplicationAccessChannel::kBleOpen:
    case ApplicationAccessChannel::kBleEncrypted:
      return true;
    case ApplicationAccessChannel::kInvalid:
      return false;
  }
  return false;
}

ApplicationSubmitResult ApplicationRequestService::submit(
    const ApplicationRequest& request) {
  if (!requesterSupported(request.requester)) {
    return ApplicationSubmitResult::kRejected;
  }
  if (response_ready_) return ApplicationSubmitResult::kBusy;

  response_ = ApplicationResponse();
  response_.requester = request.requester;
  response_.request_id = request.request_id;
  response_.kind = request.kind;

  if (!requestKindSupported(request.kind)) {
    response_.code = ApplicationResponseCode::kUnsupported;
    response_ready_ = true;
    return ApplicationSubmitResult::kAccepted;
  }

  if (!accessAllowed(request.kind, request.access)) {
    response_.code = ApplicationResponseCode::kAccessDenied;
    response_ready_ = true;
    return ApplicationSubmitResult::kAccepted;
  }

  switch (request.kind) {
    case ApplicationRequestKind::kGetConfig: {
      response_.code = ApplicationResponseCode::kOk;
      const config_format::Config& config = config_store_.config();
      response_.payload.config.backend_ready = config_store_.ready() ? 1U : 0U;
      response_.payload.config.has_committed_record =
          config_store_.hasCommittedRecord() ? 1U : 0U;
      response_.payload.config.tracking_interval_seconds =
          config.tracking_interval_seconds;
      response_.payload.config.battery_capacity_mah =
          config.battery_capacity_mah;
      break;
    }

    case ApplicationRequestKind::kGetDeviceStatus:
      if (status_snapshot_ == nullptr) {
        response_.code = ApplicationResponseCode::kUnavailable;
      } else {
        response_.code = ApplicationResponseCode::kOk;
        response_.payload.device.snapshot = status_snapshot_->device;
      }
      break;

    case ApplicationRequestKind::kGetTrackingStatus:
      if (status_snapshot_ == nullptr) {
        response_.code = ApplicationResponseCode::kUnavailable;
      } else {
        response_.code = ApplicationResponseCode::kOk;
        response_.payload.tracking.snapshot = status_snapshot_->tracking;
      }
      break;

    case ApplicationRequestKind::kGetGeofenceStatus:
      if (status_snapshot_ == nullptr) {
        response_.code = ApplicationResponseCode::kUnavailable;
      } else {
        response_.code = ApplicationResponseCode::kOk;
        response_.payload.geofence.snapshot = status_snapshot_->geofence;
      }
      break;

    case ApplicationRequestKind::kGetStorageStatus:
      if (status_snapshot_ == nullptr) {
        response_.code = ApplicationResponseCode::kUnavailable;
      } else {
        response_.code = ApplicationResponseCode::kOk;
        response_.payload.storage.snapshot = status_snapshot_->storage;
      }
      break;
  }

  response_ready_ = true;
  return ApplicationSubmitResult::kAccepted;
}

bool ApplicationRequestService::takeResponse(
    ApplicationRequester requester, ApplicationResponse& response) {
  if (!response_ready_ || response_.requester != requester) return false;
  response = response_;
  response_ready_ = false;
  response_ = ApplicationResponse();
  return true;
}

bool ApplicationRequestService::discardResponse(ApplicationRequester requester) {
  if (!response_ready_ || response_.requester != requester) return false;
  response_ready_ = false;
  response_ = ApplicationResponse();
  return true;
}

}  // namespace orun_tlp
