#pragma once
#include "config_format.h"
#include "config_incarnation_source.h"
#include "flash_backend.h"

namespace orun_tlp {

enum class ConfigTokenState : uint8_t {
  kUnavailable,
  kValid,
  kUncertain,
};

// Durable desired configuration owner for the fixed two-page ConfigStore
// partition.
//
// Runtime schema v2 seals semantic config and its application state token in one
// 48-byte record. Normal saves remain A/B ping-pong: inactive page erase,
// body+CRC stage/readback, commit-last/readback, then RAM publication.
//
// Current product scope has no deployed ConfigStore-v1 fleet. Legacy v1 evidence
// is detected only to fail closed into maintenance/reset; it is never
// automatically migrated or adopted as runtime semantic state.
class ConfigStore {
 public:
  struct Diagnostics {
    uint32_t saves = 0;
    uint32_t save_failures = 0;
    uint32_t recovery_corruptions = 0;
    uint32_t rejected_candidates = 0;
    uint32_t skipped_unchanged = 0;
    uint32_t blocked_pending_result = 0;
    uint32_t baseline_commits = 0;
    uint32_t baseline_failures = 0;
    uint32_t legacy_pages_seen = 0;
    uint32_t maintenance_lockouts = 0;
    uint32_t unreconciled_mutation_faults = 0;
    uint32_t recovery_reconciliations = 0;
  };

  // A null incarnation source is an intentional fail-closed mode used by
  // read-only host tests and models CSPRNG unavailability: defaults remain
  // readable, but a blank partition cannot establish a v2 token/baseline and
  // semantic mutation stays disabled.
  explicit ConfigStore(FlashBackend& backend,
                       ConfigIncarnationSource* incarnation_source = nullptr)
      : flash_(backend), incarnation_source_(incarnation_source) {}

  // Recover v2 state. If both pages are erased and a CSPRNG source is
  // available, establish generation=1 / revision=1 synchronously before
  // SoftDevice startup. Legacy/corrupt/unsupported evidence is never erased or
  // migrated here; begin() stays available with safe defaults and reports
  // maintenanceResetRequired().
  bool begin();

  // Advances an asynchronous normal save. It also performs one read-only full
  // two-page reconciliation after a failed/unreconciled flash mutation once
  // the backend says physical ownership is reconciled.
  void poll();

  bool ready() const { return ready_; }
  // Backward-compatible application provenance used by existing GET_CONFIG:
  // false for the internal fresh v2 default baseline, true once a semantic
  // config change has been durably committed. This keeps the frozen
  // "default vs stored" application meaning independent from token metadata.
  bool hasCommittedRecord() const { return application_config_committed_; }
  bool busy() const { return job_ != Job::kNone; }
  const config_format::Config& config() const { return config_; }

  ConfigTokenState tokenState() const { return token_state_; }
  bool stateToken(config_format::StateToken& token) const {
    if (token_state_ != ConfigTokenState::kValid) return false;
    token = token_;
    return true;
  }

  bool maintenanceResetRequired() const {
    return maintenance_reset_required_;
  }

  // Internal/local semantic-save seam. Protected remote CAS admission is a
  // later application-owner slice; this method never accepts a caller-supplied
  // token. It advances the current VALID token exactly once on a successful
  // semantic change. An unchanged config is a no-op only when recovery has an
  // unambiguous semantic state.
  bool requestSave(const config_format::Config& candidate);
  bool takeSaveResult(bool& success);

  // Normal semantic reset. This is NOT the destructive development partition
  // reinitialize action: with VALID v2 state it writes defaults as the next
  // revision; while token state is invalid/maintenance it fails closed.
  bool requestReset();

  const Diagnostics& diagnostics() const { return diagnostics_; }

 private:
  enum class Job { kNone, kErase, kWrite };
  enum class BlobStep { kBody, kCommit, kVerify };

  struct RecoveredPage {
    config_format::PageInspection inspection;
    bool semantic_valid = false;
    bool tail_dirty = false;
  };

  bool recover();
  bool establishFreshBaseline();
  bool writeFreshBaseline(const config_format::V2Record& record);
  bool startSave(const config_format::Config& candidate);
  FlashOpResult writeBlob();
  void fail();
  void finishSave();
  void setMaintenance(ConfigTokenState token_state);
  void setMaintenanceFallback(const config_format::PageInspection& fallback,
                              ConfigTokenState token_state);
  void clearRecoveredRuntimeState();

  FlashBackend& flash_;
  ConfigIncarnationSource* incarnation_source_ = nullptr;

  config_format::Config config_{};
  config_format::StateToken token_{};
  ConfigTokenState token_state_ = ConfigTokenState::kUnavailable;
  bool semantic_unambiguous_ = false;
  bool maintenance_reset_required_ = false;
  bool application_config_committed_ = false;

  uint64_t generation_ = 0;
  int active_page_ = -1;

  Diagnostics diagnostics_{};
  Job job_ = Job::kNone;
  BlobStep blob_step_ = BlobStep::kBody;
  bool flash_op_awaiting_completion_ = false;
  bool ready_ = false;
  bool save_result_ready_ = false;
  bool save_success_ = false;

  bool mutation_unreconciled_ = false;
  bool recovery_pending_ = false;

  uint32_t target_page_ = 0;
  config_format::Config pending_config_{};
  config_format::StateToken pending_token_{};
  uint64_t pending_generation_ = 0;
  uint8_t blob_[config_format::kV2RecordSize]{};
};

}  // namespace orun_tlp
