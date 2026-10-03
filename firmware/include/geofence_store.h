#pragma once

#include "flash_backend.h"
#include "geofence_format.h"
#include "geofence_incarnation_source.h"
#include "storage_config.h"

namespace orun_tlp {

enum class GeofenceTokenState : uint8_t {
  kUnavailable,
  kValid,
  kUncertain,
};

enum class GeofenceResourceState : uint8_t {
  kUnavailable,
  kClear,
  kConfigured,
};

// Durable owner for one complete permitted-area resource.
//
// M6D3B deliberately owns only persistence/recovery/token semantics. It has no
// GNSS, PositionFlow, cadence, RF, BLE or M6D2 operational-state dependency.
class GeofenceStore {
 public:
  struct Diagnostics {
    uint32_t mutations = 0;
    uint32_t mutation_failures = 0;
    uint32_t rejected_candidates = 0;
    uint32_t skipped_unchanged = 0;
    uint32_t blocked_pending_result = 0;
    uint32_t baseline_commits = 0;
    uint32_t baseline_failures = 0;
    uint32_t recovery_corruptions = 0;
    uint32_t maintenance_lockouts = 0;
    uint32_t unreconciled_mutation_faults = 0;
    uint32_t recovery_reconciliations = 0;
  };

  explicit GeofenceStore(
      FlashBackend& backend,
      GeofenceIncarnationSource* incarnation_source = nullptr)
      : flash_(backend), incarnation_source_(incarnation_source) {}

  // Recover the two-page resource. A completely erased partition is not
  // interpreted as CLEAR; when entropy is available begin() writes and
  // verifies a fresh incarnation/revision=1 CLEAR baseline first.
  bool begin();
  void poll();

  bool ready() const { return ready_; }
  bool busy() const { return job_ != Job::kNone; }

  GeofenceResourceState resourceState() const { return resource_state_; }
  // M7P7H bounded status accessors. These expose only summary metadata from
  // the already-owned durable snapshot and deliberately avoid copying the
  // ~524-byte geometry into the loop task merely to answer a status query.
  uint8_t areaCount() const {
    return resource_state_ == GeofenceResourceState::kConfigured
               ? snapshot_.area_count
               : 0;
  }
  uint16_t totalVertexCount() const {
    return resource_state_ == GeofenceResourceState::kConfigured
               ? snapshot_.total_vertex_count
               : 0;
  }
  bool currentSnapshot(geofence_format::Snapshot& snapshot) const {
    if (resource_state_ == GeofenceResourceState::kUnavailable) return false;
    snapshot = snapshot_;
    return true;
  }

  GeofenceTokenState tokenState() const { return token_state_; }
  bool stateToken(geofence_format::StateToken& token) const {
    if (token_state_ != GeofenceTokenState::kValid) return false;
    token = token_;
    return true;
  }

  bool maintenanceResetRequired() const {
    return maintenance_reset_required_;
  }

  // Whole-resource semantic mutations. These are local seams only; protected
  // BLE/LoRa CAS admission is later work. Mutation requires current VALID token
  // authority. Unchanged is accepted as a no-op only while token authority is
  // VALID; UNCERTAIN never maps to ALREADY_SATISFIED.
  bool requestReplace(const GeofenceAreaSetView& area_set);
  bool requestClear();

  // success=false means this attempt was not confirmed successful; it does not
  // prove the candidate definitely did not commit. Recovery/reconciliation may
  // subsequently expose the durable state.
  bool takeMutationResult(bool& success);

  const Diagnostics& diagnostics() const { return diagnostics_; }

 private:
  enum class Job : uint8_t { kNone, kErase, kWrite };
  enum class BlobStep : uint8_t { kBody, kCommit, kVerify };

  struct RecoveredPage {
    geofence_format::PageInspection inspection;
    bool tail_dirty = false;
  };

  bool recover();
  bool establishFreshBaseline();
  bool writeFreshBaseline(const geofence_format::Record& record);
  bool requestSnapshot(const geofence_format::Snapshot& candidate);
  bool startMutation(const geofence_format::Snapshot& candidate);
  FlashOpResult writeBlob();
  void failMutation();
  void finishMutation();

  void clearRecoveredRuntimeState();
  void setUnavailableMaintenance(GeofenceTokenState token_state);
  void setSemanticFallback(const geofence_format::Record& record,
                           GeofenceTokenState token_state);

  static bool sameSnapshot(const geofence_format::Snapshot& a,
                           const geofence_format::Snapshot& b);

  FlashBackend& flash_;
  GeofenceIncarnationSource* incarnation_source_ = nullptr;

  geofence_format::Snapshot snapshot_{};
  geofence_format::StateToken token_{};
  GeofenceResourceState resource_state_ = GeofenceResourceState::kUnavailable;
  GeofenceTokenState token_state_ = GeofenceTokenState::kUnavailable;
  bool semantic_unambiguous_ = false;
  bool maintenance_reset_required_ = false;

  uint64_t generation_ = 0;
  int active_page_ = -1;

  Diagnostics diagnostics_{};
  Job job_ = Job::kNone;
  BlobStep blob_step_ = BlobStep::kBody;
  bool flash_op_awaiting_completion_ = false;
  bool ready_ = false;

  bool mutation_result_ready_ = false;
  bool mutation_success_ = false;
  bool mutation_unreconciled_ = false;
  bool recovery_pending_ = false;

  uint32_t target_page_ = 0;
  geofence_format::Snapshot pending_snapshot_{};
  geofence_format::StateToken pending_token_{};
  uint64_t pending_generation_ = 0;

  // GeofenceStore runs on the 4-KiB Arduino loop task when physically
  // qualified. Keep record-sized recovery/verification storage in the
  // process-lifetime object instead of stacking several 560+ byte buffers
  // through begin()->baseline/recovery call chains.
  uint8_t blob_[geofence_format::kRecordSize]{};
  uint8_t scratch_[geofence_format::kRecordSize]{};
  RecoveredPage recovery_pages_[storage_config::kGeofenceRegionPages]{};
  geofence_format::PageInspection inspection_scratch_{};
};

}  // namespace orun_tlp
