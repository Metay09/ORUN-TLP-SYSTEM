#pragma once
#include "device_identity.h"
#include "flash_backend.h"
#include "security_format.h"

namespace orun_tlp {

enum class SecurityState : uint8_t {
  kUnprovisioned,
  kProvisioned,
  kForeign,
  kUnsupported,
  kFault,
};

// Durable security credential + nonce/replay persistence owner.
//
// M7P6F upgrades new writes to SecurityStore format v2 while retaining exact
// read/migration compatibility with M7P6B format v1. The physical partition
// remains the same two-page A/B region. This class still implements no crypto,
// RF secure envelope, provisioning transport, command dispatch or BLE authority.
class SecurityStore {
 public:
  struct Diagnostics {
    uint32_t recovery_corruptions = 0;
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
  };

  SecurityStore(FlashBackend& critical_flash, FlashBackend& maint_flash)
      : critical_(critical_flash), maint_(maint_flash) {}

  bool begin(DeviceIdentity device_identity);
  void poll();
  bool busy() const { return job_ != Job::kNone; }
  SecurityState state() const { return state_; }
  bool ready() const { return ready_; }
  bool exhausted() const { return exhausted_; }

  bool currentCredentialId(
      uint8_t (&out)[security_format::kCredentialIdSize]) const;
  uint32_t currentKeyEpoch() const { return credential_.key_epoch; }

  bool commitCredential(
      const uint8_t (&credential_id)[security_format::kCredentialIdSize],
      uint32_t key_epoch,
      const uint8_t (&k_root)[security_format::kKRootSize]);
  bool takeCommitResult(bool& success);

  bool reserveNextTxCounter(uint64_t& counter, uint32_t& key_epoch);

  const Diagnostics& diagnostics() const { return diagnostics_; }

 private:
  enum class Job { kNone, kNewPage, kEraseOld, kReserve };
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

  FlashBackend& critical_;
  FlashBackend& maint_;
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
  uint64_t a2d_replay_bound_ = 0;
  bool exhausted_ = false;
  bool migration_needed_ = false;
  bool migration_attempted_ = false;

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
  security_format::Credential pending_credential_{};
  uint64_t pending_tx_bound_ = 0;
  uint64_t pending_a2d_bound_ = 0;
  int erase_old_page_ = -1;

  bool commit_result_ready_ = false;
  bool commit_success_ = false;

  uint8_t blob_[security_format::kCredentialRecordSize]{};
  uint32_t blob_offset_ = 0;
  uint32_t blob_size_ = 0;
};

}  // namespace orun_tlp
