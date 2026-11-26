#include "product_custody_store.h"
#include "product_region.h"

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

bool ProductCustodyStore::erasedRange(uint32_t offset, uint32_t end,
                                      bool& erased) const {
  erased = true;
  uint8_t bytes[fmt::kPageHeaderSize];
  while (offset < end) {
    const size_t size = end - offset < sizeof(bytes) ? end - offset : sizeof(bytes);
    if (!flash_.read(offset, bytes, size)) return false;
    for (size_t i = 0; i < size; ++i) {
      if (bytes[i] != 0xFFU) {
        erased = false;
        return true;
      }
    }
    offset += size;
  }
  return true;
}

bool ProductCustodyStore::isErasedPage(uint16_t page) const {
  const uint32_t base = uint32_t(page) * fmt::kPageSize;
  bool erased;
  return erasedRange(base, base + fmt::kPageSize, erased) && erased;
}

bool ProductCustodyStore::shapeValid(const uint8_t* p, size_t size) const {
  if (p==nullptr || (size!=fmt::kEventSize && size!=fmt::kPeriodicSize))
    return false;
  // Frozen TLPv2 PRODUCT_SECURE shape. Structural validation is NOT origin
  // AEAD verification: Gateway accepts opaque objects subject to a separate
  // enrolled-source/admission policy (not activated by this class).
  if (p[0]!=0x02U || p[1]!=0x04U || p[2]!=0x02U ||
      p[4]>0x03U || p[5]!=0x01U ||
#ifndef ORUN_GATEWAY_BATCH_ACK
      (p[4]&0x02U)==0U ||
#endif
      p[7]!=0U || be64(p+8U)==0U || be32(p+16U)==UINT32_MAX ||
      be64(p+20U)==0U || be64(p+20U)==UINT64_MAX)
    return false;
  if (size==fmt::kPeriodicSize)
    return p[3]==0x01U && p[6]==64U;
  return p[3]==0x02U && p[6]==48U;
}

bool ProductCustodyStore::validObject(const fmt::RecordInspection& record) const {
  return (record.evidence == fmt::RecordEvidence::kHeld ||
          record.evidence == fmt::RecordEvidence::kHandedOff) &&
         shapeValid(record.object, record.object_size);
}

bool ProductCustodyStore::scanAtBoot() {
  uint64_t generations[32]{};
  bool needs_active_witness = false;
  isolated_pages_ = 0;
  for (uint16_t page = 0; page < page_count_; ++page) {
    const uint32_t base = uint32_t(page) * fmt::kPageSize;
    uint8_t raw[fmt::kPageHeaderSize];
    fmt::PageInspection header{};
    if (!flash_.read(base, raw, sizeof(raw)) ||
        !fmt::inspectPageHeader(raw, sizeof(raw), header)) return false;

    if (header.evidence != fmt::PageEvidence::kActive) {
      bool empty_tail;
      if (!erasedRange(base + sizeof(raw), base + fmt::kPageSize, empty_tail))
        return false;
      if (empty_tail && header.evidence == fmt::PageEvidence::kErased) continue;

      // A recognisable interrupted OCQ2 allocation contained no records yet.
      // A torn erase can also destroy that identity. In that case serve only
      // if another independently validated active v3 page witnesses ownership.
      // Neither case is evidence that this page may be erased or programmed.
      if (!(empty_tail && fmt::interruptedPageOpen(raw, header))) {
        if (!fmt::possibleTornErase(raw)) return false;
        needs_active_witness = true;
      }
      isolated_pages_ |= uint32_t(1) << page;
      continue;
    }

    for (uint16_t previous = 0; previous < page; ++previous)
      if (generations[previous] == header.generation) return false;
    generations[page] = header.generation;

    uint16_t next = 0;
    for (uint16_t slot = 0; slot < fmt::kRecordsPerPage; ++slot) {
      fmt::RecordInspection record{};
      if (!readRecord(page, slot, record)) return false;
      // Every programmed slot is spent. Durable reads validate the record
      // independently; neither holes nor uncertain words are reusable here.
      if (record.evidence != fmt::RecordEvidence::kErased) next = slot + 1U;
    }
    if (header.generation > active_generation_) {
      active_generation_ = header.generation;
      active_page_ = page;
      next_slot_ = next;
    }
  }
  return !needs_active_witness || active_page_ != UINT16_MAX;
}

bool ProductCustodyStore::begin() {
  ready_=false;
  faulted_=false;
  awaiting_=false;
  phase_=Phase::kIdle;
  outcome_ready_=false;
  maintenance_ready_=false;
  maintenance_=Maintenance{};
  active_page_=UINT16_MAX;
  active_generation_=0U;
  product_region::Census census{};
  if (page_count_==0U || page_count_>32U || !flash_.begin() ||
      flash_.hasUnreconciledMutation() ||
      !product_region::takeCensus(flash_,page_count_,census) ||
      product_region::mountProductCustodyDecision(census) !=
          product_region::MountDecision::kMount || !scanAtBoot()) {
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
    if (isolatedPage(page)) continue;
    fmt::PageInspection header{};
    if (!readPage(page,header)) return false;
    if (header.evidence==fmt::PageEvidence::kErased) continue;
    if (header.evidence!=fmt::PageEvidence::kActive) return false;
    for (uint16_t slot=0;slot<fmt::kRecordsPerPage;++slot) {
      fmt::RecordInspection record{};
      if (!readRecord(page,slot,record)) return false;
      if (!validObject(record)) continue;
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
  if (!shapeValid(object, size) || !(object[4] & 0x02U)) return Admission::kRejected;
  return requestObject(object, size, duplicate);
}
#ifdef ORUN_GATEWAY_BATCH_ACK
ProductCustodyStore::Admission ProductCustodyStore::requestLive(
    const uint8_t* object, size_t size, Handle* duplicate) {
  if (!shapeValid(object, size) || (object[4] & 0x02U)) return Admission::kRejected;
  return requestObject(object, size, duplicate);
}
#endif
ProductCustodyStore::Admission ProductCustodyStore::requestObject(
    const uint8_t* object, size_t size, Handle* duplicate) {
  if (!ready() || flash_.hasUnreconciledMutation())
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
      if (isolatedPage(page)) continue;
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
  const bool maintenance = phase_==Phase::kHandoffMark ||
      phase_==Phase::kHandoffVerify || phase_==Phase::kReclaimErase ||
      phase_==Phase::kReclaimVerify;
  if (flash_.hasUnreconciledMutation()) {
    if (maintenance) finishMaintenance(false); else finish(false);
    return;
  }
  if (maintenance) {
    FlashOpResult r=FlashOpResult::kFailed;
    switch (phase_) {
      case Phase::kHandoffMark: {
        const uint32_t handed_off=0U;  // NOR: only clears bits
        r=write(handoff_offset_,&handed_off,4U);
        if (r==FlashOpResult::kDone) phase_=Phase::kHandoffVerify;
        break;
      }
      case Phase::kHandoffVerify: {
        fmt::RecordInspection found{};
        const bool ok=readRecord(pending_handle_.page,pending_handle_.slot,found) &&
            found.evidence==fmt::RecordEvidence::kHandedOff &&
            fmt::exactObject(found,pending_object_,pending_size_);
        finishMaintenance(ok);
        return;
      }
      case Phase::kReclaimErase:
        if (awaiting_) {
          r=flash_.pollPending();
          if (r!=FlashOpResult::kPending) awaiting_=false;
        } else {
          r=flash_.erasePage(pending_handle_.page);
          if (r==FlashOpResult::kPending) awaiting_=true;
        }
        if (r==FlashOpResult::kDone) phase_=Phase::kReclaimVerify;
        break;
      case Phase::kReclaimVerify: {
        const bool ok=isErasedPage(pending_handle_.page);
        finishMaintenance(ok);
        return;
      }
      default: return;
    }
    if (r==FlashOpResult::kFailed) finishMaintenance(false);
    return;
  }
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
    case Phase::kHandoffMark: case Phase::kHandoffVerify:
    case Phase::kReclaimErase: case Phase::kReclaimVerify:
      return;  // handled above
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
  return readDurable(handle,false,object,size);
}

ProductCustodyStore::Lookup ProductCustodyStore::readConfirmedDurable(
    const Handle& handle,
    uint8_t (&object)[fmt::kMaxObjectSize],size_t& size) const {
  return readDurable(handle,true,object,size);
}

ProductCustodyStore::Lookup ProductCustodyStore::readDurable(
    const Handle& handle,bool allow_handed_off,
    uint8_t (&object)[fmt::kMaxObjectSize],size_t& size) const {
  if (!ready() || busy() || flash_.hasUnreconciledMutation() ||
      handle.page>=page_count_ ||
      handle.slot>=fmt::kRecordsPerPage || handle.generation==0U)
    return Lookup::kError;
  if (isolatedPage(handle.page)) return Lookup::kNotHeld;
  fmt::PageInspection header{};
  fmt::RecordInspection record{};
  if (!readPage(handle.page,header) || !readRecord(handle.page,handle.slot,record))
    return Lookup::kError;
  if (header.evidence!=fmt::PageEvidence::kActive ||
      header.generation!=handle.generation ||
      !validObject(record)) return Lookup::kNotHeld;
  if (record.evidence!=fmt::RecordEvidence::kHeld && !allow_handed_off)
    return Lookup::kNotHeld;
  size=record.object_size;
  memcpy(object,record.object,size);
  return Lookup::kFound;
}

ProductCustodyStore::Handoff ProductCustodyStore::requestHandoff(
    const Handle& handle,const uint8_t* object,size_t size) {
  if (!ready() || flash_.hasUnreconciledMutation()) return Handoff::kRejected;
  if (busy() || outcome_ready_ || maintenance_ready_) return Handoff::kBusy;
  if (object==nullptr || size==0U || size>fmt::kMaxObjectSize ||
      handle.page>=page_count_ || handle.slot>=fmt::kRecordsPerPage ||
      handle.generation==0U)
    return Handoff::kRejected;
  if (isolatedPage(handle.page)) return Handoff::kNotFound;
  fmt::PageInspection header{};
  fmt::RecordInspection record{};
  if (!readPage(handle.page,header) || !readRecord(handle.page,handle.slot,record))
    return Handoff::kRejected;
  // Bind page generation AND exact bytes: an accept for a reclaimed and
  // reused slot (or for other bytes) must never mark a different object.
  if (header.evidence!=fmt::PageEvidence::kActive ||
      header.generation!=handle.generation ||
      !validObject(record) || !fmt::exactObject(record,object,size))
    return Handoff::kNotFound;
  if (record.evidence==fmt::RecordEvidence::kHandedOff)
    return Handoff::kAlreadyHandedOff;
  if (record.evidence!=fmt::RecordEvidence::kHeld) return Handoff::kNotFound;
  if (record.next_handoff_slot>1U) return Handoff::kExhausted;
  handoff_offset_=recordOffset(handle.page,handle.slot) +
      (record.next_handoff_slot==0U ? fmt::kRecordHandoff0Offset
                                    : fmt::kRecordHandoff1Offset);
  pending_handle_=handle;
  pending_size_=size;
  memcpy(pending_object_,object,size);
  phase_=Phase::kHandoffMark;
  return Handoff::kStarted;
}

bool ProductCustodyStore::canReclaim(uint16_t page, uint32_t reserved_pages,
                                    fmt::PageInspection& header) const {
  // Keep the newest header: it witnesses OCQ2 ownership through a torn erase
  // and prevents reusing a generation after power loss. A new committed page
  // makes its predecessor eligible.
  if (page >= page_count_ || page == active_page_ || isolatedPage(page) ||
      (reserved_pages & (uint32_t(1) << page))) return false;
  if (!readPage(page, header) || header.evidence != fmt::PageEvidence::kActive)
    return false;
  for (uint16_t slot = 0; slot < fmt::kRecordsPerPage; ++slot) {
    fmt::RecordInspection record{};
    if (!readRecord(page, slot, record) ||
        record.evidence != fmt::RecordEvidence::kHandedOff || !validObject(record))
      return false;
  }
  return true;
}

int ProductCustodyStore::reclaimablePage(uint32_t reserved_pages) const {
  if (!ready() || busy() || flash_.hasUnreconciledMutation()) return -1;
  fmt::PageInspection header{};
  for (uint16_t page = 0; page < page_count_; ++page)
    if (canReclaim(page, reserved_pages, header)) return page;
  return -1;
}

bool ProductCustodyStore::requestReclaim(uint16_t page, uint32_t reserved_pages) {
  fmt::PageInspection header{};
  if (!ready() || busy() || outcome_ready_ || maintenance_ready_ ||
      flash_.hasUnreconciledMutation() || !canReclaim(page, reserved_pages, header))
    return false;
  pending_handle_ = Handle{};
  pending_handle_.page = page;
  pending_handle_.generation = header.generation;
  awaiting_=false;
  phase_=Phase::kReclaimErase;
  return true;
}

void ProductCustodyStore::finishMaintenance(bool success) {
  maintenance_.kind = (phase_==Phase::kReclaimErase || phase_==Phase::kReclaimVerify)
      ? Maintenance::Kind::kReclaim : Maintenance::Kind::kHandoff;
  maintenance_.success=success;
  maintenance_.handle=pending_handle_;
  maintenance_ready_=true;
  phase_=Phase::kIdle;
  awaiting_=false;
  // Any failed or unverifiable program/erase fails closed until reboot. On
  // reboot a torn marker reads as HELD (retried with the other slot) and an
  // incompletely erased page is isolated when ownership is established.
  if (!success || flash_.hasUnreconciledMutation()) faulted_=true;
  memset(pending_object_,0,sizeof(pending_object_));
}

bool ProductCustodyStore::takeMaintenanceResult(Maintenance& result) {
  if (!maintenance_ready_) return false;
  result=maintenance_;
  maintenance_ready_=false;
  maintenance_=Maintenance{};
  return true;
}

}  // namespace orun_tlp
