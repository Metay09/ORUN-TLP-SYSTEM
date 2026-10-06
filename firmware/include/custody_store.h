#pragma once

#include <stddef.h>
#include <stdint.h>

#include "custody_store_format.h"
#include "flash_backend.h"

namespace orun_tlp {

// SF4B portable, transport-neutral durable custody queue foundation.
//
// The store owns only opaque current HISTORY_SECURE observation bytes and their
// local durable custody / Edge-handoff lifecycle. It does not authenticate RF,
// mint custody ACKs, decrypt History, own a physical flash partition, or talk
// to Edge/backend transports.
class CustodyStore {
 public:
  struct Handle {
    uint16_t page = UINT16_MAX;
    uint16_t slot = UINT16_MAX;
    uint64_t page_generation = 0;
  };

  struct Diagnostics {
    uint32_t recovered_held = 0;
    uint32_t recovered_handed_off = 0;
    uint32_t staged_records = 0;
    uint32_t partial_record_commits = 0;
    uint32_t uncertain_retire_markers = 0;
    uint32_t duplicate_held = 0;
    uint32_t duplicate_handed_off = 0;
    uint32_t admissions_started = 0;
    uint32_t admissions_committed = 0;
    uint32_t admission_failures = 0;
    uint32_t admission_no_capacity = 0;
    uint32_t handoffs_committed = 0;
    uint32_t handoff_failures = 0;
    uint32_t pages_prepared = 0;
    uint32_t pages_reclaimed = 0;
    uint32_t reclaim_intents_committed = 0;
    uint32_t reclaim_intent_recoveries = 0;
    uint32_t reclaim_intent_faults = 0;
    uint32_t maintenance_failures = 0;
    uint32_t recovery_faults = 0;
    uint32_t unreconciled_mutation_faults = 0;
  };

  enum class AdmissionResult : uint8_t {
    kStarted,
    kDuplicateHeld,
    kDuplicateHandedOff,
    kNoCapacity,
    kBusy,
    kRejected,
  };

  enum class MaintenanceResult : uint8_t {
    kStarted,
    kNoWork,
    kBusy,
    kRejected,
  };

  CustodyStore(FlashBackend& backend, uint16_t page_count)
      : flash_(backend), page_count_(page_count) {}

  // Recovery is read-only. SF4B deliberately does not allocate a physical
  // partition or perform an erase merely because begin() ran.
  bool begin();
  void poll();

  bool ready() const { return ready_; }
  bool busy() const { return job_ != Job::kNone; }
  bool faulted() const { return faulted_; }
  bool hasPreparedPage() const { return prepared_page_ >= 0; }
  uint16_t pageCount() const { return page_count_; }
  uint32_t slotCapacity() const {
    return uint32_t(page_count_) * custody_store_format::kRecordsPerPage;
  }

  // Caller must pass one complete structurally-admitted current 73-byte
  // HISTORY_SECURE observation. Authentication/admission policy belongs above
  // this store and is intentionally not implied by this method.
  AdmissionResult requestCustody(const uint8_t* object, size_t object_size,
                                 Handle* duplicate_handle = nullptr);
  // A newly stored object becomes ACK-eligible only after this result reports
  // success=true. kStarted itself is never durable-custody evidence.
  bool takeCustodyResult(bool& success, Handle& handle);

  // Precondition: caller has already verified authenticated durable Edge
  // acceptance for this exact object. The store rechecks handle generation,
  // exact object bytes, and current held state before persisting local reclaim
  // eligibility.
  bool requestMarkEdgeDurableAccepted(const Handle& handle,
                                      const uint8_t* object,
                                      size_t object_size);
  bool takeHandoffResult(bool& success);

  // Background-only erase/prepare path. requestCustody() never erases a page.
  // This prepares one empty committed page header for the next rollover, or
  // reclaims an old page only when every committed object on it is durably
  // handed off.
  MaintenanceResult requestMaintenance();
  bool takeMaintenanceResult(bool& success);

  bool oldestHeld(Handle& handle,
                  uint8_t object[custody_store_format::kObjectSize]) const;
  uint32_t heldCount() const;
  uint32_t handedOffCount() const;

  const Diagnostics& diagnostics() const { return diagnostics_; }

 private:
  enum class Job : uint8_t { kNone, kAdmission, kHandoff, kMaintenance };
  enum class Phase : uint8_t {
    kNone,
    kActivatePage,
    kRecordBody,
    kRecordCommit,
    kRetireRecord,
    kIntentBody,
    kIntentCommit,
    kErasePage,
    kHeaderBody,
    kHeaderCommit,
  };

  bool recover();
  bool readPage(uint16_t page,
                custody_store_format::PageInspection& inspection) const;
  bool readRecord(uint16_t page, uint16_t slot,
                  custody_store_format::RecordInspection& inspection) const;
  bool pageRecordAreaErased(uint16_t page) const;
  bool pageAllErased(uint16_t page) const;
  bool pageReclaimable(uint16_t page) const;
  bool readReclaimIntent(uint16_t page,
                         custody_store_format::ReclaimInspection& inspection) const;
  bool pageGenerationMatches(uint16_t page, uint64_t generation) const;
  bool findDuplicate(const uint8_t* object, size_t object_size,
                     Handle& handle, bool& handed_off) const;
  bool findAppendSlot(uint16_t& page, uint16_t& slot,
                      bool& needs_activation) const;
  int findErasedPage() const;
  int findRepairableBlankPage() const;
  int findReclaimablePage() const;
  bool generationUnique(uint16_t page, uint64_t generation) const;

  uint32_t pageOffset(uint16_t page) const {
    return uint32_t(page) * custody_store_format::kPageSize;
  }
  uint32_t recordOffset(uint16_t page, uint16_t slot) const {
    return pageOffset(page) + custody_store_format::kPageHeaderSize +
           uint32_t(slot) * custody_store_format::kRecordSize;
  }

  FlashOpResult programStep(uint32_t offset, const void* data, size_t size);
  FlashOpResult eraseStep(uint16_t page);
  void finishAdmission(bool success);
  void finishHandoff(bool success);
  void finishMaintenance(bool success);
  void failCurrentJob();

  FlashBackend& flash_;
  uint16_t page_count_ = 0;
  bool ready_ = false;
  bool faulted_ = false;
  bool flash_op_awaiting_completion_ = false;
  uint64_t max_generation_ = 0;
  int active_page_ = -1;
  int prepared_page_ = -1;

  Diagnostics diagnostics_{};
  Job job_ = Job::kNone;
  Phase phase_ = Phase::kNone;

  uint16_t target_page_ = UINT16_MAX;
  uint16_t target_slot_ = UINT16_MAX;
  uint64_t target_generation_ = 0;
  Handle pending_handle_{};
  uint8_t pending_object_[custody_store_format::kObjectSize]{};
  uint8_t record_blob_[custody_store_format::kRecordSize]{};
  uint8_t page_blob_[custody_store_format::kPageHeaderSize]{};
  uint8_t intent_blob_[custody_store_format::kReclaimIntentEnd -
                       custody_store_format::kReclaimIntentOffset]{};
  int intent_owner_page_ = -1;
  bool reclaim_intent_valid_ = false;
  bool reclaim_intent_blocked_ = false;
  uint16_t reclaim_target_page_ = UINT16_MAX;
  uint64_t reclaim_target_generation_ = 0;

  bool custody_result_ready_ = false;
  bool custody_result_success_ = false;
  Handle custody_result_handle_{};
  bool handoff_result_ready_ = false;
  bool handoff_result_success_ = false;
  bool maintenance_result_ready_ = false;
  bool maintenance_result_success_ = false;
};

}  // namespace orun_tlp
