#pragma once
#include "config_format.h"
#include "flash_backend.h"

namespace orun_tlp {

// M7P5: durable device configuration in the M7P1-decided partition
// (storage_config::kFutureConfigRegionStart..End, 2 pages, A/B ping-pong).
// Mirrors HistoryStore's proven flash invariants (erase-before-write,
// body/CRC-then-separate-commit-word, higher-generation-wins recovery) but
// is a standalone sibling: no shared code, state, or physical pages with
// HistoryStore/journal_format. A save always targets the currently INACTIVE
// page, so a failed/torn write can never corrupt the previously committed,
// still-active page -- the store's config()/ready() stay on the last good
// commit throughout.
class ConfigStore {
 public:
  struct Diagnostics {
    uint32_t saves = 0;
    uint32_t save_failures = 0;
    uint32_t recovery_corruptions = 0;
    uint32_t rejected_candidates = 0;
    uint32_t skipped_unchanged = 0;
    // requestSave() refused to start a new async save because a prior save's
    // result was still sitting unread in save_result_ready_. Not a flash
    // failure or invalid input -- purely an ownership/sequencing rejection.
    uint32_t blocked_pending_result = 0;
  };

  explicit ConfigStore(FlashBackend& backend) : flash_(backend) {}

  // Recovers the highest-generation validly-sealed page, or falls back to
  // defaults (tracking_interval_seconds = gnss_config::kTrackingIntervalSeconds,
  // battery_capacity_mah = 0) on blank/corrupt/unrecognized-schema flash.
  // Returns false only if the underlying flash backend itself is not ready
  // (e.g. real hardware invariant failure); config() still reads defaults
  // in that case.
  bool begin();
  void poll();  // One synchronous config step per call; safe to call every loop tick.
  bool ready() const { return ready_; }
  bool busy() const { return job_ != Job::kNone; }
  const config_format::Config& config() const { return config_; }

  // Validates the whole candidate; rejects (no flash write, no state
  // change) an out-of-range interval. A candidate that is byte/semantically
  // identical to the currently committed config is accepted as a
  // synchronous no-op (no erase/write) -- this path is explicitly exempt
  // from the unread-result rule below, since it never arms save_result_ready_
  // and so can never collide with one. Otherwise starts an async save;
  // poll() advances it and takeSaveResult() reports completion exactly
  // once. Returns false immediately if rejected outright: invalid
  // candidate, a save already in progress (busy()), or -- fail-closed
  // ownership rule -- a previous async save's result has not yet been
  // consumed via takeSaveResult(). This guarantees a caller can never read
  // save B's outcome while believing it belongs to save A: the store will
  // not even start save B until save A's result has been taken.
  bool requestSave(const config_format::Config& candidate);
  bool takeSaveResult(bool& success);
  // "Config reset": explicitly re-saves the default config. Affects only
  // this partition; never history, security, or bond storage, by
  // construction (this store can only ever address its own flash region).
  bool requestReset();

  const Diagnostics& diagnostics() const { return diagnostics_; }

 private:
  enum class Job { kNone, kErase, kWrite };
  enum class BlobStep { kBody, kCommit, kVerify };

  bool recover();
  bool startSave(const config_format::Config& candidate);
  FlashOpResult writeBlob();
  void fail();
  void finishSave();

  FlashBackend& flash_;
  config_format::Config config_{};
  uint64_t generation_ = 0;
  int active_page_ = -1;  // -1 = no committed page found at recovery; defaults in effect.
  Diagnostics diagnostics_{};
  Job job_ = Job::kNone;
  BlobStep blob_step_ = BlobStep::kBody;
  bool flash_op_awaiting_completion_ = false;
  bool ready_ = false;
  bool save_result_ready_ = false, save_success_ = false;
  uint32_t target_page_ = 0;
  config_format::Config pending_config_{};
  uint64_t pending_generation_ = 0;
  uint8_t blob_[config_format::kRecordSize]{};
};

}  // namespace orun_tlp
