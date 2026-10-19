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
  wipe(signed_ack_,sizeof(signed_ack_));
}

void GatewayLocalCustodyOwner::clearPending() {
  wipe(pending_key_,sizeof(pending_key_));
  wipe(pending_object_,sizeof(pending_object_));
  pending_scope_=GatewayCustodyAckScope{};
  pending_size_=0U;
  waiting_flash_=false;
}

GatewayLocalCustodyOwner::Submit GatewayLocalCustodyOwner::submit(
    const uint8_t* exact_object,size_t size,
    const GatewayCustodyAckScope& scope,
    const uint8_t (&scoped_key)[32]) {
  if (waiting_flash_ || ack_ready_ || store_.busy() || store_.hasOutcome())
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
      admission!=ProductCustodyStore::Admission::kDuplicateHeld)
    return Submit::kRejected;

  pending_scope_=scope;
  pending_size_=size;
  memcpy(pending_object_,exact_object,size);
  memcpy(pending_key_,scoped_key,sizeof(pending_key_));
  if (admission==ProductCustodyStore::Admission::kStarted) {
    waiting_flash_=true;
    return Submit::kStarted;
  }
  // Duplicate already-held object: re-ACK is allowed only after independently
  // rereading the exact durable bytes. NO ACK for handed-off/unknown records.
  if (!signCommitted(handle)) return Submit::kRejected;
  return Submit::kAckReady;
}

bool GatewayLocalCustodyOwner::signCommitted(
    const ProductCustodyStore::Handle& handle) {
  uint8_t found[100]{};
  size_t found_size=0U;
  if (store_.readConfirmedHeld(handle,found,found_size)!=
          ProductCustodyStore::Lookup::kFound ||
      found_size!=pending_size_ ||
      memcmp(found,pending_object_,pending_size_)!=0) {
    wipe(found,sizeof(found));
    clearPending();
    return false;
  }
  wipe(found,sizeof(found));
  uint8_t ack[custody_ack::kWireSize]{};
  const bool signed_ok=GatewayCustodyAckSigner::signAfterDurableAcceptance(
      pending_object_,pending_size_,pending_scope_,pending_key_,ack);
  clearPending();
  if (!signed_ok) { wipe(ack,sizeof(ack)); return false; }
  memcpy(signed_ack_,ack,sizeof(ack));
  wipe(ack,sizeof(ack));
  ack_ready_=true;
  return true;
}

void GatewayLocalCustodyOwner::service() {
  if (!waiting_flash_) return;
  store_.poll();
  bool committed=false;
  ProductCustodyStore::Handle handle{};
  if (!store_.takeCustodyResult(committed,handle)) return;
  if (!committed || !signCommitted(handle)) clearPending();
}

bool GatewayLocalCustodyOwner::takeAck(
    uint8_t (&output)[custody_ack::kWireSize]) {
  if (!ack_ready_) return false;
  memcpy(output,signed_ack_,sizeof(signed_ack_));
  wipe(signed_ack_,sizeof(signed_ack_));
  ack_ready_=false;
  return true;
}

}  // namespace orun_tlp
