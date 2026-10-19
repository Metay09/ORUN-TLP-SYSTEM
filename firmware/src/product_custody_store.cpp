#include "product_custody_store.h"

#include <string.h>

namespace orun_tlp {
namespace {
namespace fmt = custody_store_product_format;
uint64_t be64(const uint8_t* p) {
  uint64_t v=0;
  for (unsigned i=0;i<8U;++i) v=(v<<8U)|p[i];
  return v;
}
uint32_t be32(const uint8_t* p) {
  return (uint32_t(p[0])<<24U)|(uint32_t(p[1])<<16U)|
         (uint32_t(p[2])<<8U)|uint32_t(p[3]);
}
} // namespace

uint32_t ProductCustodyStore::recordOffset(uint16_t page, uint16_t slot) const {
  return uint32_t(page)*fmt::kPageSize + fmt::kPageHeaderSize +
         uint32_t(slot)*fmt::kRecordSize;
}

bool ProductCustodyStore::readPage(uint16_t page,
                                  fmt::PageInspection& out) const {
  uint8_t data[fmt::kPageHeaderSize]{};
  return page<page_count_ &&
         flash_.read(uint32_t(page)*fmt::kPageSize,data,sizeof(data)) &&
         fmt::inspectPageHeader(data,sizeof(data),out);
}

bool ProductCustodyStore::readRecord(uint16_t page, uint16_t slot,
                                    fmt::RecordInspection& out) const {
  uint8_t data[fmt::kRecordSize]{};
  return page<page_count_ && slot<fmt::kRecordsPerPage &&
         flash_.read(recordOffset(page,slot),data,sizeof(data)) &&
         fmt::inspectRecord(data,sizeof(data),out);
}

bool ProductCustodyStore::isErasedPage(uint16_t page) const {
  uint8_t bytes[fmt::kPageHeaderSize];
  for (uint32_t off=0;off<fmt::kPageSize;off+=sizeof(bytes)) {
    if (!flash_.read(uint32_t(page)*fmt::kPageSize+off,bytes,sizeof(bytes)))
      return false;
    for (uint8_t b:bytes) if (b!=0xFFU) return false;
  }
  return true;
}

bool ProductCustodyStore::shapeValid(const uint8_t* p, size_t size) const {
  if (p==nullptr || (size!=fmt::kEventSize && size!=fmt::kPeriodicSize))
    return false;
  // Frozen TLPv2 PRODUCT_SECURE shape. Structural validation is NOT origin
  // AEAD verification: Gateway accepts opaque objects subject to a separate
  // enrolled-source/admission policy (not activated by this class).
  if (p[0]!=0x02U || p[1]!=0x04U || p[2]!=0x02U ||
      p[4]>0x03U || (p[4]&0x02U)==0U || p[5]!=0x01U ||
      p[7]!=0U || be64(p+8U)==0U || be32(p+16U)==UINT32_MAX ||
      be64(p+20U)==0U || be64(p+20U)==UINT64_MAX)
    return false;
  if (size==fmt::kPeriodicSize)
    return p[3]==0x01U && p[6]==64U;
  return p[3]==0x02U && p[6]==48U;
}

bool ProductCustodyStore::scanAtBoot() {
  uint64_t maximum_generation=0;
  uint16_t latest_page=UINT16_MAX;
  uint16_t latest_free=fmt::kRecordsPerPage;
  for (uint16_t page=0;page<page_count_;++page) {
    fmt::PageInspection header{};
    if (!readPage(page,header)) return false;
    if (header.evidence==fmt::PageEvidence::kErased) {
      if (!isErasedPage(page)) return false;
      continue;
    }
    if (header.evidence!=fmt::PageEvidence::kActive) return false;
    if (header.generation==0U || header.generation==UINT64_MAX)
      return false;
    uint16_t first_empty=fmt::kRecordsPerPage;
    for (uint16_t slot=0;slot<fmt::kRecordsPerPage;++slot) {
      fmt::RecordInspection record{};
      if (!readRecord(page,slot,record)) return false;
      if (record.evidence==fmt::RecordEvidence::kErased) {
        if (first_empty==fmt::kRecordsPerPage) first_empty=slot;
      } else {
        if (first_empty!=fmt::kRecordsPerPage ||
            (record.evidence!=fmt::RecordEvidence::kHeld &&
             record.evidence!=fmt::RecordEvidence::kHandedOff) ||
            !shapeValid(record.object,record.object_size))
          return false;
      }
    }
    if (header.generation>maximum_generation) {
      maximum_generation=header.generation;
      latest_page=page;
      latest_free=first_empty;
    } else if (header.generation==maximum_generation) return false;
  }
  active_generation_=maximum_generation;
  active_page_=latest_page;
  next_slot_=latest_free;
  return true;
}

bool ProductCustodyStore::begin() {
  ready_=false;
  faulted_=false;
  awaiting_=false;
  phase_=Phase::kIdle;
  outcome_ready_=false;
  active_page_=UINT16_MAX;
  active_generation_=0U;
  if (page_count_==0U || page_count_>32U || !flash_.begin() ||
      flash_.hasUnreconciledMutation() || !scanAtBoot()) {
    faulted_=true;
    return false;
  }
  ready_=true;
  return true;
}

bool ProductCustodyStore::findDuplicate(const uint8_t* object, size_t size,
                                        Admission& result, Handle& handle) const {
  // Return false on any read/parse failure; never turn an uncertain duplicate
  // into permission to write a second object or to mint an ACK.
  for (uint16_t page=0;page<page_count_;++page) {
    fmt::PageInspection header{};
    if (!readPage(page,header)) return false;
    if (header.evidence==fmt::PageEvidence::kErased) continue;
    if (header.evidence!=fmt::PageEvidence::kActive) return false;
    for (uint16_t slot=0;slot<fmt::kRecordsPerPage;++slot) {
      fmt::RecordInspection record{};
      if (!readRecord(page,slot,record)) return false;
      if (record.evidence==fmt::RecordEvidence::kErased) continue;
      if (record.evidence!=fmt::RecordEvidence::kHeld &&
          record.evidence!=fmt::RecordEvidence::kHandedOff) return false;
      if (!fmt::exactObject(record,object,size)) continue;
      result=record.evidence==fmt::RecordEvidence::kHeld ?
             Admission::kDuplicateHeld : Admission::kDuplicateHandedOff;
      handle.page=page;
      handle.slot=slot;
      handle.generation=header.generation;
      return true;
    }
  }
  result=Admission::kStarted; // absent, not yet physically started
  return true;
}

ProductCustodyStore::Admission ProductCustodyStore::requestCustody(
    const uint8_t* object, size_t size, Handle* duplicate) {
  if (!ready() || !shapeValid(object,size) || flash_.hasUnreconciledMutation())
    return Admission::kRejected;
  if (busy() || outcome_ready_) return Admission::kBusy;
  Admission found=Admission::kRejected;
  Handle dup{};
  if (!findDuplicate(object,size,found,dup)) { faulted_=true; return Admission::kRejected; }
  if (found!=Admission::kStarted) {
    if (duplicate!=nullptr) *duplicate=dup;
    return found;
  }
  if (active_page_==UINT16_MAX || next_slot_>=fmt::kRecordsPerPage) {
    if (active_generation_==UINT64_MAX-1U) return Admission::kNoCapacity;
    uint16_t erased_page=UINT16_MAX;
    for (uint16_t page=0;page<page_count_;++page) {
      fmt::PageInspection header{};
      if (!readPage(page,header)) { faulted_=true; return Admission::kRejected; }
      if (header.evidence==fmt::PageEvidence::kErased) {
        if (!isErasedPage(page)) { faulted_=true; return Admission::kRejected; }
        erased_page=page;
        break;
      }
    }
    if (erased_page==UINT16_MAX) return Admission::kNoCapacity;
    pending_handle_.page=erased_page;
    pending_handle_.slot=0U;
    pending_handle_.generation=active_generation_+1U;
    new_page_=true;
    if (!fmt::encodePageHeader(pending_handle_.generation,page_blob_))
      return Admission::kRejected;
    phase_=Phase::kPageBody;
  } else {
    pending_handle_.page=active_page_;
    pending_handle_.slot=next_slot_;
    pending_handle_.generation=active_generation_;
    new_page_=false;
    phase_=Phase::kRecordBody;
  }
  pending_size_=size;
  memcpy(pending_object_,object,size);
  if (!fmt::encodeRecord(object,size,record_blob_)) {
    phase_=Phase::kIdle;
    return Admission::kRejected;
  }
  return Admission::kStarted;
}

FlashOpResult ProductCustodyStore::write(uint32_t offset,
                                        const void* bytes,size_t size) {
  if (awaiting_) {
    const FlashOpResult r=flash_.pollPending();
    if (r!=FlashOpResult::kPending) awaiting_=false;
    return r;
  }
  const FlashOpResult r=flash_.program(offset,bytes,size);
  if (r==FlashOpResult::kPending) awaiting_=true;
  return r;
}

void ProductCustodyStore::finish(bool success) {
  outcome_committed_=success;
  outcome_handle_=pending_handle_;
  outcome_ready_=true;
  phase_=Phase::kIdle;
  awaiting_=false;
  if (!success || flash_.hasUnreconciledMutation()) faulted_=true;
  memset(pending_object_,0,sizeof(pending_object_));
}

void ProductCustodyStore::poll() {
  if (!ready() || !busy()) return;
  if (flash_.hasUnreconciledMutation()) { finish(false); return; }
  const uint32_t base=uint32_t(pending_handle_.page)*fmt::kPageSize;
  const uint32_t record=recordOffset(pending_handle_.page,pending_handle_.slot);
  FlashOpResult r=FlashOpResult::kFailed;
  switch (phase_) {
    case Phase::kPageBody:
      r=write(base,page_blob_,fmt::kHeaderCommitOffset);
      if (r==FlashOpResult::kDone) phase_=Phase::kPageCommit;
      break;
    case Phase::kPageCommit: {
      const uint32_t committed=0U;
      r=write(base+fmt::kHeaderCommitOffset,&committed,4U);
      if (r==FlashOpResult::kDone) phase_=Phase::kPageActive;
      break;
    }
    case Phase::kPageActive: {
      const uint32_t active=0U;
      r=write(base+fmt::kHeaderActiveOffset,&active,4U);
      if (r==FlashOpResult::kDone) {
        fmt::PageInspection header{};
        if (!readPage(pending_handle_.page,header) ||
            header.evidence!=fmt::PageEvidence::kActive ||
            header.generation!=pending_handle_.generation) { finish(false); return; }
        phase_=Phase::kRecordBody;
      }
      break;
    }
    case Phase::kRecordBody:
      r=write(record,record_blob_,fmt::kRecordCommitOffset);
      if (r==FlashOpResult::kDone) phase_=Phase::kRecordCommit;
      break;
    case Phase::kRecordCommit: {
      const uint32_t committed=0U;
      r=write(record+fmt::kRecordCommitOffset,&committed,4U);
      if (r==FlashOpResult::kDone) phase_=Phase::kVerify;
      break;
    }
    case Phase::kVerify: {
      fmt::RecordInspection found{};
      if (!readRecord(pending_handle_.page,pending_handle_.slot,found) ||
          found.evidence!=fmt::RecordEvidence::kHeld ||
          !fmt::exactObject(found,pending_object_,pending_size_)) {
        finish(false); return;
      }
      if (new_page_) {
        active_page_=pending_handle_.page;
        active_generation_=pending_handle_.generation;
      }
      next_slot_=static_cast<uint16_t>(pending_handle_.slot+1U);
      finish(true);
      return;
    }
    case Phase::kIdle: return;
  }
  if (r==FlashOpResult::kFailed) finish(false);
}

bool ProductCustodyStore::takeCustodyResult(bool& committed,Handle& handle) {
  if (!outcome_ready_) return false;
  committed=outcome_committed_;
  handle=outcome_handle_;
  outcome_ready_=false;
  return true;
}

ProductCustodyStore::Lookup ProductCustodyStore::readConfirmedHeld(
    const Handle& handle,
    uint8_t (&object)[fmt::kMaxObjectSize],size_t& size) const {
  if (!ready() || busy() || flash_.hasUnreconciledMutation() ||
      handle.page>=page_count_ ||
      handle.slot>=fmt::kRecordsPerPage || handle.generation==0U)
    return Lookup::kError;
  fmt::PageInspection header{};
  fmt::RecordInspection record{};
  if (!readPage(handle.page,header) || !readRecord(handle.page,handle.slot,record))
    return Lookup::kError;
  if (header.evidence!=fmt::PageEvidence::kActive ||
      header.generation!=handle.generation ||
      (record.evidence!=fmt::RecordEvidence::kHeld &&
       record.evidence!=fmt::RecordEvidence::kHandedOff)) return Lookup::kNotHeld;
  if (record.evidence!=fmt::RecordEvidence::kHeld) return Lookup::kNotHeld;
  size=record.object_size;
  memcpy(object,record.object,size);
  return Lookup::kFound;
}

}  // namespace orun_tlp
