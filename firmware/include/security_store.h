#pragma once
#include "device_identity.h"
#include "flash_backend.h"
#include "security_format.h"

namespace orun_tlp {

// M7P6B state model (docs/milestones/M7P6B.md). Falls naturally out of the
// recovery algorithm below; not a speculative general framework.
enum class SecurityState : uint8_t {
  kUnprovisioned,  // blank/fully-corrupt flash: no credential exists yet.
  kProvisioned,    // a credential bound to this DeviceIdentity was recovered.
  kForeign,        // a structurally valid credential bound to a DIFFERENT
                   // DeviceIdentity was found (transplanted pages). Never
                   // adopted, never destructively rewritten, never issues
                   // TX counters.
  kUnsupported,    // recognized magic, unrecognized/newer schema version.
                   // Never destructively "repaired".
  kFault,          // underlying flash backend is unusable, or committed
                   // security state is ambiguous/corrupt such that continuing
                   // could roll nonce state backward. Protected TX fails closed.
};

// M7P6B: durable security credential + TX nonce-reservation foundation in
// the M7P1-decided partition (storage_config::kFutureSecurityRegionStart..End,
// 2 pages, A/B). See docs/architecture/ADR_M7P6_SECURITY_ARCHITECTURE.md and
// docs/milestones/M7P6B.md for the full design record. A standalone sibling
// of ConfigStore/HistoryStore: no shared code, state or physical pages.
// Implements no cryptography, no secure RF envelope, no provisioning
// transport and no commands -- only the durable credential/counter
// foundation those later milestones require.
//
// Two FlashBackend ports address the same physical partition (normally both
// views of one FlashMutationGate security client, or the identical fake
// backend in host tests): `critical_flash` for operations that block a
// protected TX or credential establishment (every TX_RESERVE append, and
// the page write for a brand-new credential), `maint_flash` for non-urgent
// housekeeping (compaction of a page carrying an UNCHANGED credential
// forward, and erasing a now-superseded old page). SecurityStore itself
// decides which port a given internal step uses; callers never choose.
class SecurityStore {
 public:
  struct Diagnostics {
    uint32_t recovery_corruptions = 0;
    uint32_t commits = 0;
    uint32_t commit_failures = 0;
    uint32_t reservations = 0;
    uint32_t reservation_failures = 0;
    uint32_t compactions = 0;
    uint32_t old_page_erase_failures = 0;
    uint32_t tx_counters_issued = 0;
    uint32_t exhausted_events = 0;
  };

  SecurityStore(FlashBackend& critical_flash, FlashBackend& maint_flash)
      : critical_(critical_flash), maint_(maint_flash) {}

  // Recovers the highest-generation structurally valid page. Blank/
  // fully-corrupt flash recovers as kUnprovisioned -- this does NOT disable
  // TLP v1, RF, GNSS or role behavior, and does NOT write a credential or
  // create production secrets automatically. A structurally valid page bound
  // to a DIFFERENT device_identity recovers as kForeign. A recognized-magic
  // page with an unrecognized/newer version recovers as kUnsupported.
  // Returns false only if the underlying flash backend itself cannot be
  // initialized/read. Structurally ambiguous committed security state can
  // recover as kFault with begin()==true so the composition root can report
  // the condition while legacy TLP v1 remains unaffected; protected TX still
  // fails closed.
  bool begin(DeviceIdentity device_identity);
  void poll();  // One synchronous security step per call; safe every loop tick.
  bool busy() const { return job_ != Job::kNone; }
  SecurityState state() const { return state_; }
  bool ready() const { return ready_; }
  // A credential can become permanently exhausted (TX counter space would
  // overflow) without becoming kFault -- it remains an identified, valid
  // credential; it simply can no longer issue TX counters. Only a fresh
  // commitCredential() (a new credential lifetime) can recover from this. No
  // rollover/wraparound is implemented (see the ADR).
  bool exhausted() const { return exhausted_; }

  // Only meaningful when state() == kProvisioned. Never exposes K_root --
  // there is no ordinary key read-back API in this store, by design.
  bool currentCredentialId(uint8_t (&out)[security_format::kCredentialIdSize]) const;
  uint32_t currentKeyEpoch() const { return credential_.key_epoch; }

  // INTERNAL ONLY: never reachable from a USB/BLE/LoRa command path. No
  // production firmware call site exists for this in M7P6B -- production
  // firmware never auto-generates or auto-provisions a credential. Commits a
  // brand-new credential lifetime (first provisioning from kUnprovisioned, or
  // re-provisioning from kProvisioned -- both always mint a fresh page, never
  // an in-place overwrite). Re-provisioning must use a new credential_id and
  // a root different from the currently active root; otherwise it is refused
  // so resetting the TX counter to zero cannot reuse the current key/lifetime.
  // Refused (returns false, no flash write) from
  // kForeign/kUnsupported/kFault -- this store never silently adopts or
  // "repairs" those states -- or while busy(), or while a previous commit's
  // result is unread.
  bool commitCredential(const uint8_t (&credential_id)[security_format::kCredentialIdSize],
                        uint32_t key_epoch,
                        const uint8_t (&k_root)[security_format::kKRootSize]);
  bool takeCommitResult(bool& success);

  // Returns the next TX-safe (counter, key_epoch) pair for the current
  // credential. Never returns a counter that is not already durably
  // reserved (see docs/milestones/M7P6B.md's 5-step reservation contract):
  // the durable bound is written, physically completed, verified by
  // readback, and only then does RAM expose counters below it. Returns
  // false -- and issues nothing -- unless state() == kProvisioned, not
  // exhausted(), !busy(), and a durably reserved, unconsumed counter is
  // already available; the caller must poll() and retry rather than block.
  bool reserveNextTxCounter(uint64_t& counter, uint32_t& key_epoch);

  const Diagnostics& diagnostics() const { return diagnostics_; }

 private:
  enum class Job { kNone, kNewPage, kEraseOld, kReserve };
  enum class Phase {
    kErasePage,
    kWriteHeader,
    kWriteCredential,
    kWriteReserve,
    kActivatePage,
    kEraseOldPage
  };
  enum class BlobStep { kBody, kCommit, kVerify };

  struct PageMeta {
    uint64_t generation = 0;
    uint32_t tx_reserve_used = 0;
  };

  bool recover();
  bool startNewPage(bool critical, bool seed_reserve,
                    const security_format::Credential& credential);
  bool startEraseOld();
  bool startReservation();
  void maybeAutoReserve();
  void startBlob(uint32_t offset, const uint8_t* bytes, uint32_t size);
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
  bool exhausted_ = false;

  Job job_ = Job::kNone;
  Phase phase_ = Phase::kErasePage;
  BlobStep blob_step_ = BlobStep::kBody;
  bool flash_op_awaiting_completion_ = false;
  FlashBackend* active_port_ = nullptr;  // which port this job's steps use.

  uint32_t target_page_ = 0, target_slot_ = 0;
  uint64_t target_generation_ = 0;
  bool seed_reserve_ = false;
  bool reserve_after_new_page_ = false;
  security_format::Credential pending_credential_{};
  uint64_t pending_tx_bound_ = 0;
  int erase_old_page_ = -1;

  bool commit_result_ready_ = false, commit_success_ = false;

  uint8_t blob_[security_format::kCredentialRecordSize]{};
  uint32_t blob_offset_ = 0, blob_size_ = 0;
};

}  // namespace orun_tlp
