#include "gateway_local_custody_owner.h"

#include <string.h>

namespace orun_tlp {
namespace {
void wipe(void* pointer, size_t length) {
  volatile uint8_t* p=static_cast<volatile uint8_t*>(pointer);
  while (length--!=0U) *p++=0U;
}
uint64_t be64(const uint8_t* p) {
  uint64_t result=0U;
  for(unsigned i=0;i<8U;++i) result=(result<<8U)|p[i];
  return result;
}
uint32_t be32(const uint8_t* p) {
  return (uint32_t(p[0])<<24U)|(uint32_t(p[1])<<16U)|
         (uint32_t(p[2])<<8U)|uint32_t(p[3]);
}
}  // namespace

GatewayLocalCustodyOwner::~GatewayLocalCustodyOwner() {
  clearPending();
  batch_handle_=ProductCustodyStore::Handle{};
  batch_ready_=false;
}

void GatewayLocalCustodyOwner::clearPending() {

  wipe(pending_object_,sizeof(pending_object_));
  pending_size_=0U;
  waiting_flash_=false;
}

GatewayLocalCustodyOwner::Submit GatewayLocalCustodyOwner::submit(
    const uint8_t* exact_object,size_t size,
    const GatewayCustodyAckScope& scope,
    const uint8_t (&scoped_key)[32], bool batch_mode) {
  // Product admission is exclusively 0x0A, including one object.
  // Reject any obsolete mode before touching durable storage.
  if (!batch_mode) return Submit::kRejected;
  (void)scoped_key;

  if (waiting_flash_ || batch_ready_ ||
      store_.busy() || store_.hasOutcome())
    return Submit::kBusy;
  if (!store_.ready() || exact_object==nullptr ||
      (size!=84U && size!=100U) ||
      scope.tracker_device_id==0U || scope.gateway_device_id==0U ||
      scope.key_epoch==UINT32_MAX || scope.custody_floor==UINT32_MAX ||
      scope.grant_generation==0U || scope.grant_generation==UINT32_MAX ||
      be64(exact_object+8U)!=scope.tracker_device_id ||
      be32(exact_object+16U)!=scope.key_epoch)
    return Submit::kRejected;

  ProductCustodyStore::Handle handle{};
  const ProductCustodyStore::Admission admission=
      store_.requestCustody(exact_object,size,&handle);
  if (admission==ProductCustodyStore::Admission::kBusy)
    return Submit::kBusy;
  if (admission!=ProductCustodyStore::Admission::kStarted &&
      admission!=ProductCustodyStore::Admission::kDuplicateHeld &&
      admission!=ProductCustodyStore::Admission::kDuplicateHandedOff)
    return Submit::kRejected;


  pending_size_=size;
  memcpy(pending_object_,exact_object,size);
  if (admission==ProductCustodyStore::Admission::kStarted) {
    waiting_flash_=true;
    return Submit::kStarted;
  }
  // Duplicate of a durably committed object (still held, or since handed off
  // to an Edge that durably accepted it): the Tracker's ACK was lost. Re-ACK
  // only after independently rereading the exact durable bytes; never for an
  // unknown record. Without this a lost ACK would pin the object in the
  // Tracker's 4-slot custody cache until ring retention erased it (SF5L5).
  if (!confirmCommitted(handle)) return Submit::kRejected;
  return Submit::kAckReady;
}

bool GatewayLocalCustodyOwner::confirmCommitted(
    const ProductCustodyStore::Handle& handle) {
  uint8_t found[100]{};
  size_t found_size=0U;
  if (store_.readConfirmedDurable(handle,found,found_size)!=
          ProductCustodyStore::Lookup::kFound ||
      found_size!=pending_size_ ||
      memcmp(found,pending_object_,pending_size_)!=0) {
    wipe(found,sizeof(found));
    clearPending();
    return false;
  }
  wipe(found,sizeof(found));
  // One exact durable proof, one product ACK protocol.
  clearPending();
  batch_handle_=handle;
  batch_ready_=true;
  return true;
}

void GatewayLocalCustodyOwner::service() {
  if (!waiting_flash_) return;
  store_.poll();
  bool committed=false;
  ProductCustodyStore::Handle handle{};
  if (!store_.takeCustodyResult(committed,handle)) return;
  if (!committed || !confirmCommitted(handle)) clearPending();
}

bool GatewayLocalCustodyOwner::takeBatchHandle(
    ProductCustodyStore::Handle& output) {
  if (!batch_ready_) return false;
  output=batch_handle_;
  batch_handle_=ProductCustodyStore::Handle{};
  batch_ready_=false;
  return true;
}

}  // namespace orun_tlp
