#include "gateway_custody_authorized_ingress.h"

#include <string.h>

#include "gateway_local_custody_owner.h"
#include "tlp_v2_product_secure.h"

namespace orun_tlp {
namespace {
void wipe(void* memory, size_t length) {
  volatile uint8_t* p = static_cast<volatile uint8_t*>(memory);
  while (length--) *p++ = 0U;
}
bool validScope(const GatewayCustodyAckScope& scope) {
  return scope.tracker_device_id != 0U &&
         scope.gateway_device_id != 0U &&
         scope.key_epoch != UINT32_MAX &&
         scope.custody_floor != UINT32_MAX &&
         scope.grant_generation != 0U &&
         scope.grant_generation != UINT32_MAX;
}
}  // namespace

bool GatewayCustodyAuthorizedIngress::sameScope(
    const GatewayCustodyAckScope& a, const GatewayCustodyAckScope& b) {
  return a.tracker_device_id == b.tracker_device_id &&
         a.gateway_device_id == b.gateway_device_id &&
         a.key_epoch == b.key_epoch &&
         a.custody_floor == b.custody_floor &&
         a.grant_generation == b.grant_generation;
}

void GatewayCustodyAuthorizedIngress::clearPending() {
  pending_ = false;
  pending_scope_ = GatewayCustodyAckScope{};
}

GatewayCustodyAuthorizedIngress::Result
GatewayCustodyAuthorizedIngress::receive(const uint8_t* object,
                                          size_t length,
                                          bool batch_mode) {
  if (pending_ || custody_.waitingFlash())
    return Result::kBusy;
  if (gateway_id_ == 0U || object == nullptr ||
      (length != tlp::kEventFrameSize &&
       length != tlp::kPeriodicFrameSize))
    return Result::kDenied;

  tlp::ProductSecureFrame frame{};
  if (tlp::decodeProductSecureFrame(object,length,&frame) !=
          tlp::ProductSecureCodecResult::kOk ||
      frame.device_id == 0U || frame.key_epoch == UINT32_MAX ||
      (frame.path_flags & tlp::kProductPathFlagCustodyRequested) == 0U)
    return Result::kDenied;

  GatewayCustodyAckScope scoped{};
  uint8_t key[32]{};
  const bool authorized = grants_.activeGrant(
      frame.device_id,frame.key_epoch,scoped,key);
  const bool matches = authorized && validScope(scoped) &&
      scoped.tracker_device_id == frame.device_id &&
      scoped.key_epoch == frame.key_epoch &&
      scoped.gateway_device_id == gateway_id_;
  if (!matches) { wipe(key,sizeof(key)); return Result::kDenied; }

  const GatewayLocalCustodyOwner::Submit status =
      custody_.submit(object,length,scoped,key,batch_mode);
  wipe(key,sizeof(key));
  if (status == GatewayLocalCustodyOwner::Submit::kBusy)
    return Result::kBusy;
  if (status == GatewayLocalCustodyOwner::Submit::kRejected)
    return Result::kDenied;
  pending_scope_ = scoped;
  pending_ = true;
  return status == GatewayLocalCustodyOwner::Submit::kAckReady
             ? Result::kAckReady : Result::kStarted;
}

void GatewayCustodyAuthorizedIngress::service() {
  if (!pending_) return;
  custody_.service();
  if (!custody_.waitingFlash() &&
      !custody_.batchReady()) clearPending();
}

bool GatewayCustodyAuthorizedIngress::batchReady() const {
  return pending_ && custody_.batchReady();
}

bool GatewayCustodyAuthorizedIngress::takeAuthorizedBatchHandle(
    ProductCustodyStore::Handle& handle,
    GatewayCustodyAckScope& scope) {
  if (!pending_ || !custody_.batchReady()) return false;
  // Current identity/epoch/floor/generation must still match what was
  // accepted BEFORE any durable write. Never return an unapproved handle.
  GatewayCustodyAckScope current{};
  uint8_t key[32]{};
  const bool active=grants_.activeGrant(
      pending_scope_.tracker_device_id,pending_scope_.key_epoch,current,key);
  wipe(key,sizeof(key));
  const bool authorized=active && validScope(current) &&
      current.gateway_device_id==gateway_id_ &&
      sameScope(current,pending_scope_);
  ProductCustodyStore::Handle candidate{};
  if (!custody_.takeBatchHandle(candidate)) return false;
  if (authorized) {
    handle=candidate;
    scope=current;
  }
  clearPending();
  return authorized;
}

}  // namespace orun_tlp
