#pragma once

#include <stddef.h>
#include <stdint.h>

#include "custody_store_format.h"
#include "flash_backend.h"

namespace orun_tlp {

class CustodyStore {
 public:
  struct Handle {
    constexpr Handle() = default;
    constexpr Handle(uint16_t page_value, uint16_t slot_value,
                     uint64_t generation_value)
        : page(page_value), slot(slot_value),
          page_generation(generation_value) {}

    uint16_t page = UINT16_MAX;
    uint16_t slot = UINT16_MAX;
    uint64_t page_generation = 0;
  };

  struct Diagnostics {
    uint32_t recovered_held = 0;
    uint32_t recovered_handed_off = 0;
    uint32_t staged_records = 0;
    uint32_t partial_record_commits = 0;
    uint32_t uncertain_handoff_markers = 0;
    uint32_t duplicate_held = 0;
    uint32_t duplicate_handed_off = 0;
    uint32_t admissions_started = 0;
    uint32_t admissions_committed = 0;
    uint32_t admission_failures = 0;
    uint32_t admission_no_capacity = 0;
    uint32_t handoffs_committed = 0;
    uint32_t handoff_failures = 0;
    uint32_t handoff_marker_exhausted = 0;
    uint32_t pages_prepared = 0;
    uint32_t pages_reclaimed = 0;
    uint32_t reclaim_intents_committed = 0;
    uint32_t reclaim_intents_completed = 0;
    uint32_t reclaim_intent_recoveries = 0;
    uint32_t reclaim_intent_staged = 0;
    uint32_t reclaim_intent_partial_commits = 0;
    uint32_t reclaim_intent_partial_completions = 0;
    uint32_t reclaim_intent_faults = 0;
    uint32_t reclaim_intent_slots_exhausted = 0;
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
    kIntentSlotsExhausted,
    kBusy,
    kRejected,
  };

  enum class HeldLookupResult : uint8_t {
    kFound,
    kNone,
    kReadError,
  };

  CustodyStore(FlashBackend& backend, uint16_t page_count)
      : flash_(backend), page_count_(page_count) {}

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

  AdmissionResult requestCustody(const uint8_t* object, size_t object_size,
                                 Handle* duplicate_handle = nullptr);
  bool takeCustodyResult(bool& success, Handle& handle);

  bool requestMarkEdgeDurableAccepted(const Handle& handle,
                                      const uint8_t* object,
                                      size_t object_size);
  bool takeHandoffResult(bool& success);

  MaintenanceResult requestMaintenance();
  bool takeMaintenanceResult(bool& success);

  HeldLookupResult oldestHeld(
      Handle& handle,
      uint8_t object[custody_store_format::kObjectSize]) const;
  bool heldCount(uint32_t& count) const;
  bool handedOffCount(uint32_t& count) const;

  const Diagnostics& diagnostics() const { return diagnostics_; }

 private:
  enum class Job : uint8_t { kNone, kAdmission, kHandoff, kMaintenance };
  enum class Phase : uint8_t {
    kNone,
    kActivatePage,
    kRecordBody,
    kRecordCommit,
    kHandoffMarker,
    kIntentBody,
    kIntentCommit,
    kErasePage,
    kHeaderBody,
    kHeaderCommit,
    kIntentComplete,
  };
  enum class DuplicateLookup : uint8_t {
    kNotFound,
    kHeld,
    kHandedOff,
    kReadError,
  };

  bool recover();
  bool readPage(uint16_t page,
                custody_store_format::PageInspection& inspection) const;
  bool readRecord(uint16_t page, uint16_t slot,
                  custody_store_format::RecordInspection& inspection) const;
  bool readReclaimIntent(
      uint16_t page, uint16_t slot,
      custody_store_format::ReclaimInspection& inspection) const;
  bool pageRecordAreaErased(uint16_t page) const;
  bool pagePayloadErased(uint16_t page) const;
  bool pageAllErased(uint16_t page) const;
  bool pageReclaimable(uint16_t page) const;
  bool pageGenerationMatches(uint16_t page, uint64_t generation) const;
  bool generationUnique(uint16_t page, uint64_t generation,
                        bool& unique) const;
  DuplicateLookup findDuplicate(const uint8_t* object, size_t object_size,
                                Handle& handle) const;
  bool findAppendSlot(uint16_t& page, uint16_t& slot,
                      bool& needs_activation) const;
  int findErasedPage() const;
  int findRepairableBlankPage() const;
  int findReclaimablePage() const;
  int findEmptyIntentSlot(uint16_t page) const;

  uint32_t pageOffset(uint16_t page) const {
    return uint32_t(page) * custody_store_format::kPageSize;
  }
  uint32_t recordOffset(uint16_t page, uint16_t slot) const {
    return pageOffset(page) + custody_store_format::kPageHeaderSize +
           uint32_t(slot) * custody_store_format::kRecordSize;
  }
  uint32_t intentOffset(uint16_t page, uint16_t slot) const {
    return pageOffset(page) + custody_store_format::kIntentAreaOffset +
           uint32_t(slot) * custody_store_format::kIntentSlotSize;
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
  uint8_t intent_blob_[custody_store_format::kIntentSlotSize]{};

  int intent_owner_page_ = -1;
  uint16_t intent_slot_ = UINT16_MAX;
  bool maintenance_uses_intent_ = false;
  bool reclaim_intent_valid_ = false;
  uint16_t reclaim_target_page_ = UINT16_MAX;
  uint64_t reclaim_target_generation_ = 0;

  uint32_t handoff_word_offset_ = 0;

  bool custody_result_ready_ = false;
  bool custody_result_success_ = false;
  Handle custody_result_handle_{};
  bool handoff_result_ready_ = false;
  bool handoff_result_success_ = false;
  bool maintenance_result_ready_ = false;
  bool maintenance_result_success_ = false;
};

}  // namespace orun_tlp
