#ifdef ORUN_EXPERIMENTAL_BATCH_ACK
#include "gateway_product_live_ingress.h"

#include <string.h>

#include "tlp_v2_product_secure.h"

namespace orun_tlp {
namespace {

void wipe(void* memory, size_t length) {
  volatile uint8_t* p = static_cast<volatile uint8_t*>(memory);
  while (length-- != 0U) *p++ = 0U;
}

}  // namespace

void GatewayProductLiveIngress::reset() {
  pending_ = false;
  pending_size_ = 0U;
  wipe(pending_object_, sizeof(pending_object_));
}

bool GatewayProductLiveIngress::readBackMatches(
    const ProductCustodyStore::Handle& handle,
    const uint8_t* frame, size_t size) const {
  if (!frame || size > sizeof(pending_object_)) return false;
  uint8_t confirmed[custody_store_product_format::kMaxObjectSize]{};
  size_t confirmed_size = 0U;
  const auto lookup = store_.readConfirmedDurable(
      handle, confirmed, confirmed_size);
  const bool same =
      lookup == ProductCustodyStore::Lookup::kFound &&
      confirmed_size == size && memcmp(confirmed, frame, size) == 0;
  wipe(confirmed, sizeof(confirmed));
  return same;
}

GatewayProductLiveIngress::Receive GatewayProductLiveIngress::receive(
    const uint8_t* raw, size_t size) {
  if (pending_ || store_.busy() || store_.hasOutcome())
    return Receive::kBusy;
  if (!raw || gateway_id_ == 0U ||
      (size != tlp::kPeriodicFrameSize &&
       size != tlp::kEventFrameSize)) return Receive::kDenied;
  tlp::ProductSecureFrame frame{};
  if (tlp::decodeProductSecureFrame(raw, size, &frame) !=
          tlp::ProductSecureCodecResult::kOk ||
      frame.device_id == 0U ||
      frame.key_epoch == UINT32_MAX ||
      (frame.path_flags & tlp::kProductPathFlagCustodyRequested) != 0U)
    return Receive::kDenied;

  GatewayCustodyAckScope scope{};
  uint8_t secret[32]{};
  const bool allowed = grants_.activeGrant(
      frame.device_id, frame.key_epoch, scope, secret);
  wipe(secret, sizeof(secret));
  if (!allowed || scope.tracker_device_id != frame.device_id ||
      scope.gateway_device_id != gateway_id_ ||
      scope.key_epoch != frame.key_epoch ||
      scope.custody_floor == UINT32_MAX ||
      scope.grant_generation == 0U ||
      scope.grant_generation == UINT32_MAX) return Receive::kDenied;

  ProductCustodyStore::Handle duplicate{};
  const auto admission = store_.requestLive(raw, size, &duplicate);
  if (admission == ProductCustodyStore::Admission::kDuplicateHeld ||
      admission == ProductCustodyStore::Admission::kDuplicateHandedOff) {
    return readBackMatches(duplicate, raw, size)
               ? Receive::kDuplicateDurable : Receive::kDenied;
  }
  if (admission == ProductCustodyStore::Admission::kBusy)
    return Receive::kBusy;
  if (admission != ProductCustodyStore::Admission::kStarted)
    return Receive::kDenied;

  memcpy(pending_object_, raw, size);
  pending_size_ = size;
  pending_ = true;
  return Receive::kStarted;
}

GatewayProductLiveIngress::Service GatewayProductLiveIngress::service() {
  if (!pending_) return Service::kNone;
  store_.poll();
  bool durable = false;
  ProductCustodyStore::Handle handle{};
  if (!store_.takeCustodyResult(durable, handle)) return Service::kNone;

  // No delivery report is sent: only the backend may later authenticate the
  // plaintext and issue a receipt bound to the actual record identity.
  const bool verified = durable && readBackMatches(
      handle, pending_object_, pending_size_);
  reset();
  return verified ? Service::kDurable : Service::kFailed;
}

}  // namespace orun_tlp

#endif
