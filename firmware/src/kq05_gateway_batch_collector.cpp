#include "kq05_gateway_batch_collector.h"

#include <string.h>

#include "tlp_v2_product_secure.h"

namespace orun_tlp {
namespace {
namespace ack = kq05_gateway_batch_ack;
namespace fmt = custody_store_product_format;

void wipe(void* ptr, size_t n) {
  volatile uint8_t* p=static_cast<volatile uint8_t*>(ptr);
  while(n--!=0U) *p++=0U;
}
bool sameHandle(const ProductCustodyStore::Handle& a,
                const ProductCustodyStore::Handle& b) {
  return a.page==b.page && a.slot==b.slot &&
         a.generation==b.generation;
}
bool scopeValid(const GatewayCustodyAckScope& s,
                uint64_t tracker, uint64_t gateway, uint32_t epoch) {
  return tracker!=0U && tracker!=UINT64_MAX &&
         gateway!=0U && gateway!=UINT64_MAX &&
         epoch!=UINT32_MAX &&
         s.tracker_device_id==tracker &&
         s.gateway_device_id==gateway &&
         s.key_epoch==epoch &&
         s.custody_floor!=UINT32_MAX &&
         s.grant_generation!=0U &&
         s.grant_generation!=UINT32_MAX;
}
}  // namespace

void Kq05GatewayBatchCollector::clearFlashPending() {
  wipe(pending_object_,sizeof(pending_object_));
  pending_size_=0U;
  waiting_flash_=false;
}
void Kq05GatewayBatchCollector::resetGroup() {
  wipe(handles_,sizeof(handles_));
  count_=0U;
  tracker_id_=0U;
  key_epoch_=UINT32_MAX;
  flush_requested_=false;
  prepared_=false;
  clearFlashPending();
}

bool Kq05GatewayBatchCollector::addCommitted(
    const ProductCustodyStore::Handle& handle,
    const uint8_t* expected, size_t length) {
  if (count_>=ack::kMaxObjects || expected==nullptr ||
      (length!=fmt::kPeriodicSize && length!=fmt::kEventSize) ||
      !store_.ready() || store_.busy() || store_.hasOutcome() ||
      store_.hasMaintenanceResult()) return false;

  uint8_t actual[fmt::kMaxObjectSize]{};
  size_t n=0U;
  const bool good=store_.readConfirmedDurable(handle,actual,n)==
      ProductCustodyStore::Lookup::kFound && n==length &&
      memcmp(actual,expected,n)==0;
  wipe(actual,sizeof(actual));
  if (!good) return false;

  for (uint8_t i=0U;i<count_;++i)
    if (sameHandle(handles_[i],handle)) return true;
  handles_[count_++]=handle;
  if (count_==ack::kMaxObjects) flush_requested_=true;
  return true;
}

Kq05GatewayBatchCollector::Receive Kq05GatewayBatchCollector::receive(
    const uint8_t* object, size_t length) {
  if (waiting_flash_ || prepared_ || flush_requested_ ||
      count_>=ack::kMaxObjects || store_.busy() ||
      store_.hasOutcome() || store_.hasMaintenanceResult())
    return Receive::kBusy;
  if (!store_.ready() || gateway_id_==0U ||
      gateway_id_==UINT64_MAX || object==nullptr ||
      (length!=fmt::kPeriodicSize && length!=fmt::kEventSize))
    return Receive::kDenied;

  tlp::ProductSecureFrame decoded{};
  if (tlp::decodeProductSecureFrame(object,length,&decoded)!=
          tlp::ProductSecureCodecResult::kOk ||
      decoded.device_id==0U || decoded.device_id==UINT64_MAX ||
      decoded.key_epoch==UINT32_MAX ||
      (decoded.path_flags & tlp::kProductPathFlagCustodyRequested)==0U)
    return Receive::kDenied;
  if (count_!=0U &&
      (tracker_id_!=decoded.device_id || key_epoch_!=decoded.key_epoch))
    return Receive::kBusy;  // Never mix scopes or silently discard group.

  GatewayCustodyAckScope grant{};
  uint8_t key[32]{};
  const bool authorized=grants_.activeGrant(
      decoded.device_id,decoded.key_epoch,grant,key) &&
      scopeValid(grant,decoded.device_id,gateway_id_,decoded.key_epoch);
  wipe(key,sizeof(key));
  if (!authorized) return Receive::kDenied;

  ProductCustodyStore::Handle handle{};
  const ProductCustodyStore::Admission admission=
      store_.requestCustody(object,length,&handle);
  if (admission==ProductCustodyStore::Admission::kBusy)
    return Receive::kBusy;
  if (admission!=ProductCustodyStore::Admission::kStarted &&
      admission!=ProductCustodyStore::Admission::kDuplicateHeld &&
      admission!=ProductCustodyStore::Admission::kDuplicateHandedOff)
    return Receive::kDenied;

  if (count_==0U) {
    tracker_id_=decoded.device_id;
    key_epoch_=decoded.key_epoch;
  }
  if (admission==ProductCustodyStore::Admission::kStarted) {
    memcpy(pending_object_,object,length);
    pending_size_=length;
    waiting_flash_=true;
    return Receive::kStartedFlash;
  }

  for (uint8_t i=0U;i<count_;++i)
    if (sameHandle(handles_[i],handle)) return Receive::kDuplicate;
  if (!addCommitted(handle,object,length)) {
    if (count_==0U) resetGroup();
    return Receive::kDenied;
  }
  return Receive::kQueued;
}

Kq05GatewayBatchCollector::Service Kq05GatewayBatchCollector::service() {
  if (!waiting_flash_) return Service::kIdle;
  store_.poll();
  bool committed=false;
  ProductCustodyStore::Handle handle{};
  if (!store_.takeCustodyResult(committed,handle))
    return Service::kWaitingFlash;

  bool valid=false;
  if (committed)
    valid=addCommitted(handle,pending_object_,pending_size_);
  clearFlashPending();
  if (!valid) {
    // Previously readback-confirmed objects stay queued; a failed
    // *different* record can NEVER be covered by the existing ACK.
    if (count_==0U) resetGroup();
    return Service::kRejected;
  }
  return Service::kQueued;
}

bool Kq05GatewayBatchCollector::requestFlush() {
  if (count_==0U || waiting_flash_) return false;
  flush_requested_=true;
  return true;
}

Kq05GatewayBatchCollector::Prepare
Kq05GatewayBatchCollector::prepare(
    uint8_t (&wire)[ack::kMaxSize], size_t& length) {
  if (!flush_requested_ || count_==0U || waiting_flash_)
    return Prepare::kNotReady;
  if (!store_.ready() || store_.busy() ||
      store_.hasOutcome() || store_.hasMaintenanceResult())
    return Prepare::kStorageBusy;
  // Recreate and reauthorize each attempt; do not cache an ACK that could
  // remain "prepared" after the Gateway's grant was revoked or rotated.
  uint8_t candidate[ack::kMaxSize]{};
  size_t n=0U;
  const auto result=signer_.issue(
      tracker_id_,key_epoch_,handles_,count_,candidate,n);
  if (result==Kq05GatewayDurableBatchOwner::Result::kBusy)
    return Prepare::kStorageBusy;
  if (result!=Kq05GatewayDurableBatchOwner::Result::kReady ||
      n>sizeof(wire)) {
    wipe(candidate,sizeof(candidate));
    prepared_=false;
    return Prepare::kDenied;
  }
  memcpy(wire,candidate,n);
  length=n;
  wipe(candidate,sizeof(candidate));
  prepared_=true;
  return Prepare::kReady;
}

bool Kq05GatewayBatchCollector::markTxAdmitted() {
  // Caller must own the RF admission result; no storage claim is inferred
  // from a prepared packet that never reached its TX queue.
  if (!prepared_ || !flush_requested_ || waiting_flash_ ||
      count_==0U) return false;
  resetGroup();
  return true;
}

}  // namespace orun_tlp
