#pragma once

#include <stddef.h>
#include <stdint.h>

#include "flash_backend.h"
#include "observation_incarnation_source.h"
#include "observation_store_format.h"

namespace orun_tlp {

namespace osf = observation_store_format;

class ObservationStore {
 public:
  static constexpr uint16_t kMaxPages = 32U;

  struct Handle {
    uint16_t page = UINT16_MAX;
    uint16_t slot = UINT16_MAX;
    uint64_t page_generation = 0;
    osf::RecordIdentity identity{};
  };

  struct Record {
    Handle handle{};
    osf::RecordKind kind = osf::RecordKind::kPeriodic;
    uint8_t schema = 0;
    uint8_t payload_size = 0;
    bool released = false;
    uint8_t payload[osf::kDataPayloadSize]{};
  };

  struct Diagnostics {
    uint32_t recovered_retained = 0;
    uint32_t recovered_released = 0;
    uint32_t staged_records = 0;
    uint32_t partial_record_commits = 0;
    uint32_t recovery_faults = 0;
    uint32_t append_started = 0;
    uint32_t append_committed = 0;
    uint32_t append_failures = 0;
    uint32_t releases_committed = 0;
    uint32_t release_failures = 0;
    uint32_t release_marker_exhausted = 0;
    uint32_t pages_prepared = 0;
    uint32_t pages_activated = 0;
    uint32_t unreconciled_mutation_faults = 0;
  };

  enum class AppendResult : uint8_t {
    kStarted,
    kNoCapacity,
    kBusy,
    kRejected
  };
  enum class MaintenanceResult : uint8_t {
    kStarted,
    kNoWork,
    kBusy,
    kRejected
  };
  enum class LookupResult : uint8_t { kFound, kNone, kReadError };

  ObservationStore(FlashBackend& backend, uint16_t page_count,
                   ObservationIncarnationSource* incarnation_source)
      : flash_(backend),
        page_count_(page_count),
        incarnation_source_(incarnation_source) {}

  bool begin(uint64_t device_id);
  void poll();

  bool ready() const { return ready_; }
  bool busy() const { return job_ != Job::kNone; }
  bool faulted() const { return faulted_; }
  uint16_t pageCount() const { return page_count_; }
  uint16_t dataPageCount() const {
    return page_count_ > osf::kControlPageCount
               ? static_cast<uint16_t>(page_count_ - osf::kControlPageCount)
               : 0U;
  }
  uint64_t incarnation() const { return incarnation_; }
  const Diagnostics& diagnostics() const { return diagnostics_; }

  bool peekNextIdentity(osf::RecordIdentity& identity) const;
  AppendResult requestAppend(osf::RecordKind kind, uint8_t schema,
                             const uint8_t* payload, size_t payload_size);
  bool takeAppendResult(bool& success, Handle& handle);

  bool requestRelease(const osf::RecordIdentity& identity);
  bool takeReleaseResult(bool& success);

  MaintenanceResult requestMaintenance();
  bool takeMaintenanceResult(bool& success);
  bool hasPreparedDataPage() const { return prepared_data_page_ >= 0; }

  LookupResult lookup(const osf::RecordIdentity& identity,
                      Record& record) const;
  LookupResult oldestRetained(Record& record) const;
  bool retainedCount(uint32_t& count) const;
  bool releasedCount(uint32_t& count) const;

 private:
  enum class Job : uint8_t { kNone, kAppend, kRelease, kMaintenance };
  enum class Phase : uint8_t {
    kNone,
    kActivatePage,
    kRecordBody,
    kRecordCommit,
    kReleaseMarker,
    kHeaderBody,
    kHeaderCommit,
  };

  struct PageSummary {
    uint16_t page = UINT16_MAX;
    uint64_t generation = 0;
    uint32_t first_sequence = 0;
    uint32_t last_sequence = 0;
  };

  uint32_t pageOffset(uint16_t page) const {
    return uint32_t(page) * osf::kPageSize;
  }
  uint32_t recordOffset(uint16_t page, uint16_t slot) const {
    return pageOffset(page) + osf::kPageHeaderSize +
           uint32_t(slot) * osf::kDataRecordSize;
  }

  bool recover();
  bool readPage(uint16_t page, osf::PageInspection& inspection) const;
  bool readRecord(uint16_t page, uint16_t slot,
                  osf::RecordInspection& inspection) const;
  bool pageAllErased(uint16_t page) const;
  bool pagePayloadErased(uint16_t page) const;
  bool pageGenerationMatches(uint16_t page, uint64_t generation) const;
  bool findAppendSlot(uint16_t& page, uint16_t& slot,
                      bool& needs_activation) const;
  int findErasedDataPage() const;
  LookupResult findRecord(const osf::RecordIdentity& identity,
                          Record& out) const;
  bool countByRelease(bool released, uint32_t& count) const;

  FlashOpResult programStep(uint32_t offset, const void* data, size_t size);
  void finishAppend(bool success);
  void finishRelease(bool success);
  void finishMaintenance(bool success);
  void failCurrentJob();

  FlashBackend& flash_;
  uint16_t page_count_ = 0;
  ObservationIncarnationSource* incarnation_source_ = nullptr;

  bool ready_ = false;
  bool faulted_ = false;
  bool flash_op_awaiting_completion_ = false;
  uint64_t device_id_ = 0;
  uint64_t incarnation_ = 0;
  uint32_t next_sequence_ = 1U;
  uint64_t max_data_generation_ = 0;
  int active_data_page_ = -1;
  int prepared_data_page_ = -1;

  Diagnostics diagnostics_{};
  Job job_ = Job::kNone;
  Phase phase_ = Phase::kNone;

  uint16_t target_page_ = UINT16_MAX;
  uint16_t target_slot_ = UINT16_MAX;
  uint64_t target_generation_ = 0;
  uint32_t release_word_offset_ = 0;
  Handle pending_handle_{};
  osf::RecordKind pending_kind_ = osf::RecordKind::kPeriodic;
  uint8_t pending_schema_ = 0;
  uint8_t pending_payload_size_ = 0;
  uint8_t pending_payload_[osf::kDataPayloadSize]{};
  uint8_t record_blob_[osf::kDataRecordSize]{};
  uint8_t page_blob_[osf::kPageHeaderSize]{};

  bool append_result_ready_ = false;
  bool append_result_success_ = false;
  Handle append_result_handle_{};
  bool release_result_ready_ = false;
  bool release_result_success_ = false;
  bool maintenance_result_ready_ = false;
  bool maintenance_result_success_ = false;
};

}  // namespace orun_tlp
