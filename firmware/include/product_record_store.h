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
//   mounted store is faulted, with no pending job/cache outcome or unreconciled
//   flash mutation. The caller reboots afterwards to mount.
//
// Nothing is sealed or sent here; TLP v1 POSITION and HistoryStore stay the
// live path until the SF5F RF step.
class CustodyAckReleaseOwner;

class ProductRecordStore {
 public:
  enum class Submit : uint8_t { kAccepted, kNotMounted, kBusy };

  // SF5J2a: Exact protected-byte cache admission for retained observations.
  // kStarted is NOT durable: the caller must wait for takeCacheOutcome().
  enum class CacheSubmit : uint8_t {
    kStarted, kAlreadyDurable, kNotMounted, kBusy, kNoCapacity, kRejected
  };

  struct CacheOutcome {
    osf::RecordIdentity identity{};
    bool durable = false;
  };

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
    uint32_t exact_cache_committed = 0;
    uint32_t exact_cache_failed = 0;
    uint32_t custody_release_committed = 0;
    uint32_t custody_release_failed = 0;
  };

  ProductRecordStore(FlashBackend& flash, uint16_t page_count,
                     ObservationIncarnationSource* source)
      : flash_(flash), page_count_(page_count),
        store_(flash, page_count, source) {}

  void begin(uint64_t device_id, const product_region::Jobs& jobs);
  void poll();

  Submit submitPeriodic(const PeriodicRecordInputs& inputs);
  bool takeOutcome(Outcome& outcome);

  // Read-only candidate selectors; no RF and no custody authority.
  ObservationStore::LookupResult oldestRetained(
      ObservationStore::Record& record) const;
  ObservationStore::ControlLookupResult findCachedExact(
      const osf::RecordIdentity& identity, osc::ExactObject& value) const;

  // Caller supplies a previously sealed exact frame. Structural checks bind
  // local device, supported family and custody intent. Flash success and
  // readback must finish before kStarted can become durable.
  CacheSubmit submitExactCache(const osc::ExactObject& value);
  bool takeCacheOutcome(CacheOutcome& outcome);

  // Read-only release preflight evidence for a future authenticated Gateway
  // custody ACK. Requires a retained logical record and byte-identical,
  // durably cached exact protected object. This is NOT ACK authorization:
  // it checks neither Gateway credentials nor HMAC and releases NOTHING.
  // Fail closed while the product store is mutating or needs recovery.
  bool custodyCandidateMatchesRetainedExact(
      const osc::ExactObject& expected) const;

  // Enumerate at most four durably cached exact objects for a future
  // authenticated 0x05/0x06 custody ACK matcher. Read-only and fail-closed.
  // This is NOT a custody/ACK admission or a record release.
  bool snapshotCustodyCandidates(
      osc::ExactObject (&objects)[osf::kMaxExactCustodyObjects],
      uint16_t& count) const;

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
  friend class CustodyAckReleaseOwner;

  // Only the HMAC-verifying custody owner may initiate this mutation.
  // It carries one previously authenticated exact object and is checked
  // against the live retained record again before starting any flash work.
  bool requestAuthenticatedCustodyRelease(const osc::ExactObject& expected);
  bool takeCustodyReleaseOutcome(osf::RecordIdentity& identity,
                                 bool& durable);
  void finishCustodyRelease(bool durable);

  enum class Job : uint8_t {
    kNone, kAppend, kMaintenance, kControl, kExactCache, kCacheMaintenance,
    kCustodyRelease
  };
  enum class PrepareState : uint8_t { kIdle, kErasing, kWaiting };
  static constexpr uint8_t kMaxMaintenanceRounds = 4U;

  void startNext();
  void runMaintenance();
  void finishPending(bool stored, const osf::RecordIdentity& identity);
  void drivePrepare();
  void endPrepare(bool success);
  CacheSubmit startCache(bool allow_control_maintenance);
  void finishCache(bool durable);
  bool matchesCachedObject(const osc::ExactObject& expected) const;

  FlashBackend& flash_;
  uint16_t page_count_;
  ObservationStore store_;
  product_region::Owner owner_ = product_region::Owner::kNone;
  product_region::MountDecision decision_ =
      product_region::MountDecision::kNoOwner;
  product_region::Census census_{0U, 0U, 0U, 0U};
  bool region_readable_ = false;
  bool mounted_ = false;
  uint64_t device_id_ = 0;

  bool pending_ = false;
  PeriodicRecordInputs pending_inputs_{};
  osf::RecordIdentity pending_identity_{};
  Job job_ = Job::kNone;
  uint8_t maintenance_rounds_ = 0;

  bool outcome_ready_ = false;
  Outcome outcome_{};

  bool cache_pending_ = false;
  bool cache_outcome_ready_ = false;
  CacheOutcome cache_outcome_{};
  bool release_outcome_ready_ = false;
  bool release_outcome_durable_ = false;
  osf::RecordIdentity release_identity_{};
  osc::ExactObject cache_object_{};

  PrepareState prepare_state_ = PrepareState::kIdle;
  uint16_t prepare_page_ = 0;
  bool prepare_result_ready_ = false;
  bool prepare_success_ = false;

  Diagnostics diagnostics_{};
};

}  // namespace orun_tlp
