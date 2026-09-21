#include "application_request.h"

#include "config_store.h"

namespace orun_tlp {

ApplicationSubmitResult ApplicationRequestService::submit(
    const ApplicationRequest& request) {
  if (response_ready_) return ApplicationSubmitResult::kBusy;

  response_ = ApplicationResponse();
  response_.requester = request.requester;
  response_.request_id = request.request_id;

  switch (request.kind) {
    case ApplicationRequestKind::kGetConfig:
      response_.code = ApplicationResponseCode::kOk;
      response_.config_backend_ready = config_store_.ready();
      response_.config_has_committed_record =
          config_store_.hasCommittedRecord();
      response_.config = config_store_.config();
      break;
    default:
      // Unknown typed operations fail closed at the application boundary.
      // No config/flash/radio/security side effect is performed.
      response_.code = ApplicationResponseCode::kUnsupported;
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
