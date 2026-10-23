#pragma once
#include <stddef.h>
#include <stdint.h>

#include "flash_backend.h"
#include "security_format.h"
#include "security_v3_format.h"

// SF5K3H: production-source, physically mutating v3 A/B page transaction.
// SF5K3I integrates it through SecurityStore's explicit local migration
// and v3 compaction owner, never through RF/BLE/automatic v2 boot upgrade.
// Only same-credential, byte-identical policy + exact counter snapshots
// from an independently verified v2/v3 SOURCE are eligible. No grants,
// revokes, root rotations, new provisioning or ACK release operations.
// SecurityStore now owns admission, arbitration and crash recovery;
// this transaction engine still grants no in-RAM Gateway ACK authority.
namespace orun_tlp {
namespace security_v3_page_writer {

enum class Status : uint8_t { kIdle, kBusy, kCommitted, kFault };

class SnapshotTransaction {
 public:
  explicit SnapshotTransaction(FlashBackend& exclusive_flash)
      : flash_(exclusive_flash) {}

  // Caller must hold EXCLUSIVE, initialized/quiescent flash ownership.
  // start() reads and confirms the existing authoritative source on the
  // opposite A/B page before scheduling ANY destination mutation.
  // The caller-supplied credential must match source ID/epoch/root;
  // snapshot must exactly preserve TX/A2D and all grant/tombstone state.
  // Only v2->v3 migration or v3->v3 identical-policy compaction supported.
  // This is not a public provisioning/policy mutation boundary.
  bool start(uint8_t destination_page,
             const security_format::Credential& credential,
             const security_v3_format::Snapshot& preserved);

  // At most one physical operation/verification phase per call.
  // kPending is reconciled via pollPending(), never resubmitted.
  // On ANY failure the transaction becomes kFault and cannot retry.
  // Source A/B page is NEVER erased by this class.
  Status poll();
  // A complete transaction may be reused for a later compaction; a FAULT
  // remains permanently latched until a fresh SecurityStore boot/owner.
  bool rearmAfterCommit();
  Status status() const { return status_; }
  uint32_t physicalMutationCompletions() const { return completed_ops_; }

 private:
  enum class Phase : uint8_t {
    kErase,
    kHeaderBody,
    kCredential,
    kRecords,
    kPreactivationCheck,
    kActivate,
    kFinished
  };
  enum class Step : uint8_t { kBody, kCommit };

  bool snapshotFromSource(uint8_t page,
                          security_v3_format::Snapshot& out) const;
  bool buildRecord(uint32_t index);
  bool compareSourceSnapshot(
      const security_v3_format::Snapshot& source,
      const security_v3_format::Snapshot& requested) const;
  bool verifyBeforeActivate() const;
  bool verifyCommittedHeader() const;
  bool verifiedRead(uint32_t offset, const uint8_t* bytes,
                    size_t length) const;
  FlashOpResult executeProgram(uint32_t offset, const uint8_t* bytes,
                               uint32_t length);
  void fault();
  void nextRecord();

  FlashBackend& flash_;
  Status status_ = Status::kIdle;
  Phase phase_ = Phase::kErase;
  Step step_ = Step::kBody;
  bool awaiting_ = false;
  uint8_t dest_ = 0U;
  uint64_t generation_ = 0U;
  security_format::Credential credential_{};
  security_v3_format::Snapshot snapshot_{};
  uint8_t blob_[security_format::kCredentialRecordSize]{};
  uint32_t blob_size_ = 0U;
  uint32_t blob_offset_ = 0U;
  uint32_t record_index_ = 0U;
  uint32_t record_total_ = 0U;
  uint32_t completed_ops_ = 0U;
};

}  // namespace security_v3_page_writer
}  // namespace orun_tlp
