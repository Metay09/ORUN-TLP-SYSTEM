#pragma once

#include <stdint.h>

#include "flash_backend.h"
#include "observation_store.h"
#include "product_period_builder.h"
#include "product_region.h"

namespace orun_tlp {

// SF5F step 1: the tracker's durable PERIODIC records on the product region
// (docs/milestones/SF5.md). One owner composes ObservationStore with the D2
// mount decision and drives it from the loop:
//
// - begin() runs at boot, before SoftDevice starts (the synchronous backend
//   refuses afterwards). It mounts only an erased region or the store's own
//   pages; mounting writes nothing.
// - submitPeriodic() takes one finished report period. The record gets the
//   store's next identity when it is actually appended, so the identity in
//   the plaintext always equals the stored one.
// - poll() appends, and when the store has no room runs maintenance first
//   (control maintenance first when the store resumes an interrupted
//   compaction after a reset, SF5C LOW-1). Each record's outcome is reported
//   once through takeOutcome().
// - requestPrepare() is the only whole-region erase, allowed only when the
//   region holds nothing this store can use (unrecognised leftovers) or the
//   mounted store is faulted. The caller reboots afterwards to mount.
//
// Nothing is sealed or sent here; TLP v1 POSITION and HistoryStore stay the
// live path until the SF5F RF step.
class ProductRecordStore {
 public:
  enum class Submit : uint8_t { kAccepted, kNotMounted, kBusy };

  struct Outcome {
    uint64_t incarnation = 0;
    uint32_t sequence = 0;
    bool stored = false;
  };

  struct Diagnostics {
    uint32_t submitted = 0;
    uint32_t stored = 0;
    uint32_t failed = 0;
    uint32_t dropped_busy = 0;
    uint32_t maintenance_runs = 0;
  };

  ProductRecordStore(FlashBackend& flash, uint16_t page_count,
                     ObservationIncarnationSource* source)
      : flash_(flash), page_count_(page_count),
        store_(flash, page_count, source) {}

  void begin(uint64_t device_id, const product_region::Jobs& jobs);
  void poll();

  Submit submitPeriodic(const PeriodicRecordInputs& inputs);
  bool takeOutcome(Outcome& outcome);

  bool requestPrepare();
  bool preparing() const { return prepare_state_ != PrepareState::kIdle; }
  bool takePrepareResult(bool& success);

  bool regionReadable() const { return region_readable_; }
  bool mounted() const { return mounted_; }
  bool pending() const { return pending_; }
  product_region::Owner owner() const { return owner_; }
  product_region::MountDecision decision() const { return decision_; }
  const product_region::Census& census() const { return census_; }
  const ObservationStore& store() const { return store_; }
  const Diagnostics& diagnostics() const { return diagnostics_; }

 private:
  enum class Job : uint8_t { kNone, kAppend, kMaintenance, kControl };
  enum class PrepareState : uint8_t { kIdle, kErasing, kWaiting };
  static constexpr uint8_t kMaxMaintenanceRounds = 4U;

  void startNext();
  void runMaintenance();
  void finishPending(bool stored, const osf::RecordIdentity& identity);
  void drivePrepare();
  void endPrepare(bool success);

  FlashBackend& flash_;
  uint16_t page_count_;
  ObservationStore store_;
  product_region::Owner owner_ = product_region::Owner::kNone;
  product_region::MountDecision decision_ =
      product_region::MountDecision::kNoOwner;
  product_region::Census census_{0U, 0U, 0U, 0U};
  bool region_readable_ = false;
  bool mounted_ = false;

  bool pending_ = false;
  PeriodicRecordInputs pending_inputs_{};
  osf::RecordIdentity pending_identity_{};
  Job job_ = Job::kNone;
  uint8_t maintenance_rounds_ = 0;

  bool outcome_ready_ = false;
  Outcome outcome_{};

  PrepareState prepare_state_ = PrepareState::kIdle;
  uint16_t prepare_page_ = 0;
  bool prepare_result_ready_ = false;
  bool prepare_success_ = false;

  Diagnostics diagnostics_{};
};

}  // namespace orun_tlp
