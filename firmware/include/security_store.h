#pragma once
#include "device_identity.h"
#include "flash_backend.h"
#include "security_format.h"
#include "security_v3_format.h"
#include "security_v3_page_writer.h"

namespace orun_tlp {

class HistorySecureCrypto;

enum class SecurityState : uint8_t {
  kUnprovisioned,
  kProvisioned,
  kForeign,
  kUnsupported,
  kFault,
};

// SF5K3E keeps its strict read-only physical V3 inspection API.
// SF5K3I separately supports verified V3 live SecurityStore recovery and
// local v2->v3 maintenance migration; the inspector itself never grants
// Gateway ACK admission or mutates flash.
enum class SecurityV3Inspection : uint8_t {
  kNotV3,
  kStaged,
  kVerified,
  kForeign,
  kCorrupt,
  kReadFault
};

struct SecurityV3ReadOnlySummary {
  uint64_t page_generation = 0;
  uint64_t tx_reserved_bound = 0;
  uint64_t a2d_replay_bound = 0;
  uint32_t key_epoch = 0;
  uint32_t custody_policy_floor = 0;
  uint32_t policy_revision = 0;
  uint32_t max_grant_generation = 0;
  uint8_t active_gateway_count = 0;
};

// Durable security credential + nonce/replay persistence owner.
//
// M7P6F writes v2 and retains v1 migration; SF5K3I adds live validated
// v3 recovery/counter reserves and an EXPLICIT v2->v3 local migration.
// New provisioning still writes v2, and no radio/BLE path invokes migration.
// The physical two-page A/B partition remains unchanged.
//
// M7P6I permits only the narrow HistorySecureCrypto friend to use the active
// in-RAM root credential for the frozen SF2 protected-History operation.
// SF5B adds the equally narrow ProductSecureCrypto friend: PRODUCT_SECURE
// profile 0x01 reuses exactly the same root D2A construction and TX counter
// (ORUN_TLP_V2_PRODUCT_SECURE_WIRE.md 3.2), seal only. There
// is still no public/remote K_root readback API, RF secure runtime,
// provisioning transport, command dispatch or BLE authority here.
class SecurityStore {
 public:
  struct Diagnostics {
    uint32_t recovery_corruptions = 0;
    uint32_t recovery_burned_slots = 0;
    uint32_t commits = 0;
    uint32_t commit_failures = 0;
    uint32_t reservations = 0;
    uint32_t reservation_failures = 0;
    uint32_t compactions = 0;
    uint32_t migrations = 0;
    uint32_t migration_failures = 0;
    uint32_t old_page_erase_failures = 0;
    uint32_t tx_counters_issued = 0;
    uint32_t exhausted_events = 0;
    uint32_t a2d_admissions = 0;
    uint32_t a2d_rejections = 0;
    uint32_t a2d_reservations = 0;
    uint32_t a2d_reservation_failures = 0;
    uint32_t a2d_exhausted_events = 0;
    uint32_t activation_ambiguities = 0;
    uint32_t append_inspection_failures = 0;
    uint32_t unreconciled_mutation_faults = 0;
    uint32_t mutation_failure_lockouts = 0;
    uint32_t v3_policy_commits = 0;
    uint32_t v3_policy_failures = 0;
  };

  SecurityStore(FlashBackend& critical_flash, FlashBackend& maint_flash)
      : critical_(critical_flash), maint_(maint_flash),
        v3_writer_(maint_flash) {}

  bool begin(DeviceIdentity device_identity);
  void poll();
  // Local maintenance admission only; no radio/BLE caller. Requires idle,
  // provisioned v2 SecurityStore and an externally quiescent flash owner.
  // Never automatically upgrades existing field devices just on boot.
  // A completed migration activates v3 counter recovery/reservations.
  bool requestV3Migration();
  bool busy() const { return job_ != Job::kNone; }
  SecurityState state() const { return state_; }
  bool ready() const { return ready_; }
  bool exhausted() const { return exhausted_; }

  // Diagnostic-only physical V3 page scan (index 0 or 1).
  // Does not touch state_, credential_, authorization, counters, or flash
  // mutation APIs; no K_root, gateway IDs or grant entries are returned.
  // Output is published ONLY on kVerified. A v3 page NEVER becomes active
  // authority through this method; begin()/poll() still use v2 semantics.
  SecurityV3Inspection inspectV3PageReadOnly(
      uint32_t page, SecurityV3ReadOnlySummary& output) const;

  bool currentCredentialId(
      uint8_t (&out)[security_format::kCredentialIdSize]) const;
  uint32_t currentKeyEpoch() const { return credential_.key_epoch; }

  bool commitCredential(
      const uint8_t (&credential_id)[security_format::kCredentialIdSize],
      uint32_t key_epoch,
      const uint8_t (&k_root)[security_format::kKRootSize]);
  bool takeCommitResult(bool& success);

  bool reserveNextTxCounter(uint64_t& counter, uint32_t& key_epoch);

  // INTERNAL AUTHENTICATED-RECEIVE BOUNDARY ONLY. No production RF/BLE caller
  // is introduced by M7P6F. The caller must invoke this only after AEAD
  // authentication/decryption for the active credential. Returns false only
  // when no admission operation could be accepted (store unavailable/busy or
  // an earlier result is unread). When it returns true, takeA2dReplayResult()
  // yields accepted/rejected either immediately (duplicate or already inside
  // the durable reserve) or after poll() durably commits a required reserve.
  // Application dispatch is forbidden until accepted==true is retrieved.
  bool submitAuthenticatedA2dCounter(
      const uint8_t (&credential_id)[security_format::kCredentialIdSize],
      uint32_t key_epoch, uint64_t counter);
  bool takeA2dReplayResult(bool& accepted);

  const Diagnostics& diagnostics() const { return diagnostics_; }

 private:
  friend class HistorySecureCrypto;
  friend class ProductSecureCrypto;
  // Only host regression defines this seam; no production implementation.
  // This is NOT a security/authentication boundary for radio or BLE.
  friend class SecurityV3PolicyTestSeam;

  enum class PolicyAction : uint8_t {
    kGrant, kRevoke, kAdvanceFloor
  };

  enum class Job {
    kNone,
    kNewPage,
    kEraseOld,
    kReserve,
    kA2dReplayReserve,
    kV3Page,
    kV3PolicyAppend
  };
  enum class NewPagePurpose {
    kCredentialCommit,
    kCompaction,
    kMigration,
  };
  enum class Phase {
    kErasePage,
    kWriteHeader,
    kWriteTxState,
    kWriteA2dState,
    kWriteCredential,
    kWriteReserve,
    kActivatePage,
    kEraseOldPage
  };
  enum class BlobStep { kBody, kCommit, kVerify };

  struct PageMeta {
    uint64_t generation = 0;
    uint8_t version = 0;
    uint32_t state_used = 0;
  };

  bool recover();
  bool startNewPage(NewPagePurpose purpose,
                    const security_format::Credential& credential);
  bool startEraseOld();
  bool startReservation();
  bool startA2dReplayReservation(uint64_t counter, uint64_t bound);
  bool startV3Page(bool migration);
  void completeV3Page();
  // PRIVATE until a separately reviewed authenticated command owner exists.
  // Caller must not infer radio ACK authority from a durable journal entry.
  bool beginV3PolicyMutation(PolicyAction action, uint8_t slot,
                             uint64_t gateway_id, uint32_t floor);
  bool startV3PolicyAppend();
  void completeV3PolicyAppend();
  // Only the future privileged owner may consume this durable result.
  bool takeV3PolicyResult(bool& committed);
  void maybeAutoReserve();
  void startBlob(uint32_t offset, const uint8_t* bytes, uint32_t size);
  void startSnapshotTxState();
  void startSnapshotA2dState();
  void startSnapshotCredential();
  FlashOpResult writeBlob();
  FlashOpResult writeBlobBodyOnly();
  FlashOpResult writePageActivation();
  void fail();
  void completeNewPage();
  void completeEraseOld(bool success);
  void completeReserve();
  void completeA2dReplayReserve();

  FlashBackend& critical_;
  FlashBackend& maint_;
  security_v3_page_writer::SnapshotTransaction v3_writer_;
  DeviceIdentity device_identity_;
  SecurityState state_ = SecurityState::kFault;
  bool ready_ = false;
  Diagnostics diagnostics_{};

  PageMeta pages_[2]{};
  int active_page_ = -1;
  uint64_t newest_generation_ = 0;

  security_format::Credential credential_{};
  uint64_t tx_reserved_bound_ = 0;
  uint64_t tx_next_ = 0;
  security_v3_format::Snapshot v3_snapshot_{};
  uint64_t a2d_replay_bound_ = 0;
  uint64_t a2d_runtime_hwm_ = 0;
  bool a2d_runtime_hwm_valid_ = false;
  bool exhausted_ = false;
  bool migration_needed_ = false;
  bool migration_attempted_ = false;
  uint8_t consecutive_mutation_failures_ = 0;

  Job job_ = Job::kNone;
  NewPagePurpose new_page_purpose_ = NewPagePurpose::kCredentialCommit;
  Phase phase_ = Phase::kErasePage;
  BlobStep blob_step_ = BlobStep::kBody;
  bool flash_op_awaiting_completion_ = false;
  FlashBackend* active_port_ = nullptr;

  uint32_t target_page_ = 0;
  uint32_t target_slot_ = 0;
  uint64_t target_generation_ = 0;
  bool reserve_after_new_page_ = false;
  bool replay_after_new_page_ = false;
  security_format::Credential pending_credential_{};
  uint64_t pending_tx_bound_ = 0;
  uint64_t pending_a2d_bound_ = 0;
  uint64_t pending_replay_counter_ = 0;
  uint64_t pending_replay_bound_ = 0;
  int erase_old_page_ = -1;
  bool v3_page_is_migration_ = false;
  bool policy_after_v3_compaction_ = false;
  security_v3_format::Entry pending_policy_entry_{};
  security_v3_format::Snapshot pending_policy_snapshot_{};
  bool v3_policy_result_ready_ = false;
  bool v3_policy_result_committed_ = false;

  bool commit_result_ready_ = false;
  bool commit_success_ = false;
  bool a2d_result_ready_ = false;
  bool a2d_result_accepted_ = false;

  uint8_t blob_[security_format::kCredentialRecordSize]{};
  uint32_t blob_offset_ = 0;
  uint32_t blob_size_ = 0;
};

}  // namespace orun_tlp
