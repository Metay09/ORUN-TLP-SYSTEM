#pragma once

#include <stddef.h>
#include <stdint.h>

#include "flash_backend.h"
#include "observation_incarnation_source.h"
#include "observation_store_format.h"
#include "observation_store_control.h"

namespace orun_tlp {

namespace osf = observation_store_format;
namespace osc = observation_store_control;

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
    bool release_uncertain = false;
    bool release_marker_exhausted = false;
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
    uint32_t pages_reclaimed = 0;
    uint32_t rotation_recoveries = 0;
    uint32_t capacity_lost_total = 0;
    uint32_t capacity_lost_periodic = 0;
    uint32_t capacity_lost_event = 0;
    uint32_t capacity_lost_result = 0;
    uint32_t control_recovered_active = 0;
    uint32_t control_recovered_tombstones = 0;
    uint32_t control_staged_records = 0;
    uint32_t control_partial_commits = 0;
    uint32_t control_writes_started = 0;
    uint32_t control_writes_committed = 0;
    uint32_t control_write_failures = 0;
    uint32_t control_pages_compacted = 0;
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
    kControlMaintenanceRequired,
    kBusy,
    kRejected
  };
  enum class LookupResult : uint8_t { kFound, kNone, kReadError };
  enum class ControlWriteResult : uint8_t {
    kStarted,
    kAlreadySatisfied,
    kNoCapacity,
    kBusy,
    kRejected
  };
  enum class ControlLookupResult : uint8_t {
    kFound,
    kNone,
    kReadError
  };

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

  // Bounded durable control state. These operations use an append-only
  // control journal; logical deletion is a tombstone record, not an in-place
  // rewrite of the authoritative payload.
  MaintenanceResult requestControlMaintenance();
  bool takeControlMaintenanceResult(bool& success);

  ControlWriteResult requestPutExactObject(const osc::ExactObject& value);
  ControlWriteResult requestClearExactObject(
      const osf::RecordIdentity& identity);
  ControlLookupResult findExactObject(
      const osf::RecordIdentity& identity, osc::ExactObject& value) const;

  ControlWriteResult requestPutOpenOccurrence(
      const osc::OpenOccurrence& value);
  ControlWriteResult requestClearOpenOccurrence(
      const osc::OpenOccurrence& key);
  ControlLookupResult findOpenOccurrence(
      const osc::OpenOccurrence& key, osc::OpenOccurrence& value) const;

  ControlWriteResult requestPutResultGuard(const osc::ResultGuard& value);
  ControlWriteResult requestClearResultGuard(const osc::ResultGuard& key);
  ControlLookupResult findResultGuard(
      const osc::ResultGuard& key, osc::ResultGuard& value) const;

  bool takeControlWriteResult(bool& success);

 private:
  enum class Job : uint8_t {
    kNone,
    kAppend,
    kRelease,
    kMaintenance,
    kControlWrite,
    kControlMaintenance
  };
  enum class Phase : uint8_t {
    kNone,
    kActivatePage,
    kRecordBody,
    kRecordCommit,
    kReleaseMarker,
    kHeaderErasePage,
    kHeaderBody,
    kHeaderCommit,
    kControlBody,
    kControlCommit,
    kControlEraseTarget,
    kControlHeaderBody,
    kControlHeaderCommit,
    kControlCopyBody,
    kControlCopyCommit,
    kControlActivate,
    kRotationIntentBody,
    kRotationIntentCommit,
    kRotationErasePage,
    kRotationHeaderBody,
    kRotationHeaderCommit,
    kRotationCompleteBody,
    kRotationCompleteCommit,
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
  uint32_t controlOffset(uint16_t page, uint16_t slot) const {
    return pageOffset(page) + osf::kPageHeaderSize +
           uint32_t(slot) * osf::kControlRecordSize;
  }

  bool recover();
  bool readPage(uint16_t page, osf::PageInspection& inspection) const;
  bool readRecord(uint16_t page, uint16_t slot,
                  osf::RecordInspection& inspection) const;
  bool readControl(uint16_t page, uint16_t slot,
                   osf::ControlInspection& inspection) const;
  bool pageAllErased(uint16_t page) const;
  bool pagePayloadErased(uint16_t page) const;
  bool pageGenerationMatches(uint16_t page, uint64_t generation) const;
  bool findAppendSlot(uint16_t& page, uint16_t& slot,
                      bool& needs_activation) const;
  int findErasedDataPage() const;
  int findReclaimableDataPage() const;
  int findEmptyControlSlot() const;
  uint16_t emptyControlSlots(bool& read_ok) const;
  bool readLatestStoreState(osc::StoreState& state, bool& found) const;
  int findOldestDataPage() const;
  bool buildRotationIntent(uint16_t page, uint64_t new_generation,
                           osc::StoreState& state) const;
  // Compare the effective committed control state when a damaged header
  // no longer carries an independently verifiable generation.
  bool sameLiveControlSnapshot(uint16_t a, uint16_t b) const;
  bool controlPayloadValid(const osf::ControlInspection& control) const;
  bool controlSameKey(const osf::ControlInspection& a,
                      const osf::ControlInspection& b) const;
  bool resultGuardLive(const osc::ResultGuard& guard,
                       bool& read_ok) const;
  bool pendingResultReservation(osf::RecordIdentity& identity,
                                bool& found,
                                uint64_t* command_id = nullptr) const;
  bool controlIsLatest(uint16_t slot,
                       const osf::ControlInspection& control,
                       bool& read_ok) const;
  ControlLookupResult findLatestControl(
      osf::ControlKind kind, const uint8_t* key_payload, size_t key_size,
      osf::ControlInspection& out) const;
  uint16_t activeControlCount(osf::ControlKind kind,
                              bool& read_ok) const;
  ControlWriteResult requestControlWrite(
      osf::ControlKind kind, uint8_t schema,
      const uint8_t* payload, size_t payload_size);
  bool prepareNextControlCopy();
  bool controlCopyFinished() const;
  LookupResult findRecord(const osf::RecordIdentity& identity,
                          Record& out) const;
  bool countByRelease(bool released, uint32_t& count) const;

  FlashOpResult programStep(uint32_t offset, const void* data, size_t size);
  FlashOpResult eraseStep(uint16_t page);
  void finishAppend(bool success);
  void finishRelease(bool success);
  void finishMaintenance(bool success);
  void finishControlWrite(bool success);
  void finishControlMaintenance(bool success);
  void failCurrentJob();
  bool mutationUncertain() const {
    return mutation_outcome_uncertain_ || flash_.hasUnreconciledMutation();
  }

  FlashBackend& flash_;
  uint16_t page_count_ = 0;
  ObservationIncarnationSource* incarnation_source_ = nullptr;

  bool ready_ = false;
  bool faulted_ = false;
  bool flash_op_awaiting_completion_ = false;
  bool mutation_outcome_uncertain_ = false;
  uint64_t device_id_ = 0;
  uint64_t incarnation_ = 0;
  uint32_t next_sequence_ = 1U;
  uint64_t max_data_generation_ = 0;
  int active_data_page_ = -1;
  int prepared_data_page_ = -1;
  int active_control_page_ = -1;
  uint64_t active_control_generation_ = 0;
  uint64_t next_control_serial_ = 1U;

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

  osf::ControlKind pending_control_kind_ = osf::ControlKind::kExactObject;
  uint8_t pending_control_schema_ = 0;
  uint16_t pending_control_payload_size_ = 0;
  uint64_t pending_control_serial_ = 0;
  uint8_t pending_control_payload_[osf::kControlPayloadSize]{};
  uint8_t control_blob_[osf::kControlRecordSize]{};
  uint16_t control_source_page_ = UINT16_MAX;
  uint16_t control_source_scan_slot_ = 0;
  uint16_t control_target_slot_ = 0;

  osc::StoreState rotation_state_{};
  osc::StoreState rotation_complete_state_{};
  uint16_t rotation_control_slot_ = UINT16_MAX;
  uint64_t rotation_state_serial_ = 0;
  bool rotation_resuming_ = false;

  bool append_result_ready_ = false;
  bool append_result_success_ = false;
  Handle append_result_handle_{};
  bool release_result_ready_ = false;
  bool release_result_success_ = false;
  bool maintenance_result_ready_ = false;
  bool maintenance_result_success_ = false;
  bool control_write_result_ready_ = false;
  bool control_write_result_success_ = false;
  bool control_maintenance_result_ready_ = false;
  bool control_maintenance_result_success_ = false;
};

}  // namespace orun_tlp
