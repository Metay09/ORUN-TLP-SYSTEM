#pragma once

#include <stddef.h>
#include <stdint.h>

#include "custody_store_product_format.h"
#include "flash_backend.h"

namespace orun_tlp {

// SF5K3P: bounded, no-eviction OCQ2/v3 PRODUCT_SECURE durable intake.
// Reuses the ONE existing product flash partition and FlashBackend gate.
// Legacy OCQ1/v2 CustodyStore is intentionally left untouched.
// Does NOT authenticate origin, provision authority or emit an ACK.
// Pending/uncertain flash and ambiguous reboot state always fail closed.
class ProductCustodyStore final {
 public:
  struct Handle {
    uint16_t page = UINT16_MAX;
    uint16_t slot = UINT16_MAX;
    uint64_t generation = 0;
  };
  enum class Admission : uint8_t {
    kStarted, kDuplicateHeld, kDuplicateHandedOff,
    kNoCapacity, kBusy, kRejected
  };
  enum class Lookup : uint8_t { kFound, kNotHeld, kError };
  // SF5L5: the next owner (Edge) has durably accepted an exact object.
  enum class Handoff : uint8_t {
    kStarted,           // marker write in progress; see takeMaintenanceResult
    kAlreadyHandedOff,  // idempotent repeat of an earlier accept
    kNotFound,          // no such held record (stale/unknown handle or bytes)
    kExhausted,         // both marker slots consumed by torn writes: stays held
    kBusy, kRejected
  };
  struct Maintenance {
    enum class Kind : uint8_t { kNone, kHandoff, kReclaim } kind = Kind::kNone;
    bool success = false;
    Handle handle{};  // kHandoff: the record; kReclaim: .page only
  };

  ProductCustodyStore(FlashBackend& flash, uint16_t page_count)
      : flash_(flash), page_count_(page_count) {}

  bool begin();
  Admission requestCustody(const uint8_t* object, size_t size,
                           Handle* duplicate = nullptr);
  void poll();
  bool takeCustodyResult(bool& committed, Handle& handle);
  // A fixed-size output buffer prevents unchecked copying of a 100-byte
  // protected frame into an undersized caller allocation.
  Lookup readConfirmedHeld(
      const Handle& handle,
      uint8_t (&object)[custody_store_product_format::kMaxObjectSize],
      size_t& size) const;
  // The same exact durable bytes, whether still held or already handed off.
  // A Tracker whose custody ACK was lost retransmits the same object; the
  // Gateway still durably holds those bytes and may re-ACK them.
  Lookup readConfirmedDurable(
      const Handle& handle,
      uint8_t (&object)[custody_store_product_format::kMaxObjectSize],
      size_t& size) const;

  // SF5L5 hand-off. The CALLER must first authenticate the next owner's
  // durable accept and bind it to these exact bytes; the store checks the
  // bytes again and programs one hand-off marker word (no erase, no rewrite).
  Handoff requestHandoff(const Handle& handle, const uint8_t* object,
                         size_t size);
  // A page whose every record is handed off holds no custody: erase it so
  // the space can be reused. Never the page still accepting records.
  // Returns the page index, or -1 if none.
  int reclaimablePage() const;
  bool requestReclaim(uint16_t page);
  bool takeMaintenanceResult(Maintenance& result);
  bool hasMaintenanceResult() const { return maintenance_ready_; }

  bool ready() const { return ready_ && !faulted_; }
  bool faulted() const { return faulted_; }
  bool busy() const { return phase_ != Phase::kIdle; }
  bool hasOutcome() const { return outcome_ready_; }

 private:
  enum class Phase : uint8_t {
    kIdle, kPageBody, kPageCommit, kPageActive,
    kRecordBody, kRecordCommit, kVerify,
    kHandoffMark, kHandoffVerify, kReclaimErase, kReclaimVerify
  };
  void finishMaintenance(bool success);
  Lookup readDurable(const Handle& handle, bool allow_handed_off,
                     uint8_t (&object)[custody_store_product_format::kMaxObjectSize],
                     size_t& size) const;
  bool scanAtBoot();
  bool isErasedPage(uint16_t page) const;
  bool readPage(uint16_t page,
      custody_store_product_format::PageInspection& out) const;
  bool readRecord(uint16_t page, uint16_t slot,
      custody_store_product_format::RecordInspection& out) const;
  bool findDuplicate(const uint8_t* object, size_t size,
                     Admission& result, Handle& handle) const;
  bool shapeValid(const uint8_t* data, size_t size) const;
  uint32_t recordOffset(uint16_t page, uint16_t slot) const;
  FlashOpResult write(uint32_t offset, const void* bytes, size_t size);
  void finish(bool success);

  FlashBackend& flash_;
  uint16_t page_count_ = 0;
  bool ready_ = false;
  bool faulted_ = false;
  bool awaiting_ = false;
  bool outcome_ready_ = false;
  bool outcome_committed_ = false;
  bool new_page_ = false;
  uint16_t active_page_ = UINT16_MAX;
  uint16_t next_slot_ = 0;
  uint64_t active_generation_ = 0;
  Phase phase_ = Phase::kIdle;
  Handle pending_handle_{};
  Handle outcome_handle_{};
  size_t pending_size_ = 0;
  uint8_t pending_object_[custody_store_product_format::kMaxObjectSize]{};
  uint8_t page_blob_[custody_store_product_format::kPageHeaderSize]{};
  uint8_t record_blob_[custody_store_product_format::kRecordSize]{};
  uint32_t handoff_offset_ = 0;
  bool maintenance_ready_ = false;
  Maintenance maintenance_{};
};

}  // namespace orun_tlp
