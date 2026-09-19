#pragma once
#include <stdint.h>
#include "flash_backend.h"
#include "storage_config.h"

namespace orun_tlp {

// M7P3 (docs/architecture/ADR_M7_PERSISTENCE_LAYOUT.md §9): the single owner
// of every mutating (program/erasePage) history flash call. Wraps
// NrfHistoryFlash byte-for-byte unchanged for the SoftDevice-disabled path --
// still the only path exercised by shipped firmware; BLE/SoftDevice
// enablement remains a later slice (M7P7). Adds a SoftDevice-enabled
// asynchronous path using the raw Nordic sd_flash_write/sd_flash_page_erase
// SVCs directly, independent of Bluefruit: at most one physical Nordic flash
// mutation is in flight through this gate at a time.
//
// M7P5: generalized to also serve ConfigStore (the M7P1-decided
// 0x0E9000..0x0EB000 partition), per the ADR §9/§10's single-owner and
// bounded-priority-admission requirements. The public API used by
// HistoryStore (begin/read/program/erasePage/pollPending/pumpEvents/
// diagnostics(), all via the inherited FlashBackend interface) is completely
// unchanged -- it still means exactly "the history client", so no existing
// caller or test needed to change. Config gets a parallel, symmetrical API
// (configPort(), a plain FlashBackend view) that ConfigStore uses exactly
// like HistoryStore uses this object directly. Internally, both clients
// share ONE in-flight-operation slot and one pumpEvents() drain of the
// single global SoftDevice event queue -- this is what actually requires a
// shared owner: two independent gate instances would each blindly drain
// that one shared queue and could discard each other's real completion
// event as "spurious" (see docs/milestones/M7P5.md). Admission is bounded
// (exactly one outstanding request per client, no heap) and prioritized:
// whenever the physical slot is free and both clients want it, History is
// admitted first (docs/architecture/ADR_M7_PERSISTENCE_LAYOUT.md §10 -- live
// store-before-send outranks config writes). A client that already owns an
// in-flight physical operation cannot be preempted -- the Nordic SVCs have
// no cancel -- so "priority" governs admission order, not interruption.
//
// M7P6B: generalized again to also serve SecurityStore (the M7P1-decided
// 0x0E7000..0x0E9000 partition), per
// docs/architecture/ADR_M7P6_SECURITY_ARCHITECTURE.md §7.1's priority
// refinement. Security submissions are tagged with one of two priority
// classes at submission time, not fixed by owner identity like
// History/Config: SEC_CRITICAL (page write for a brand-new credential, and
// every TX_RESERVE append -- these block protected TX/credential
// establishment) outranks everything, and SEC_MAINT (page erase, compaction
// preparation of a page carrying an unchanged credential forward) is
// outranked by everything, so routine security housekeeping can never starve
// live History or Config. The admission order is therefore
// SEC_CRITICAL > History > Config > SEC_MAINT, generalized in
// higherPriorityWaiting() below instead of the old two-owner special case;
// History still outranks Config exactly as before.
//
// M7P7A closes the remaining BLE-storage concurrency hole without enabling
// BLE: stock InternalFS keeps its LittleFS/cache format, but a pinned
// framework patch acquires the same physical-flash arbiter used by this gate
// before every Nordic flash mutation. Once Bluefruit owns the global SoC
// event queue, its patched SoC task forwards flash completion events into a
// bounded bridge consumed by pumpEvents(), instead of this gate racing
// Bluefruit with a second sd_evt_get() consumer. Advertising, pairing UX,
// provisioning and DFU remain later slices.
//
// M7P7A ownership-transfer invariant: once SoftDevice has *accepted* a
// physical flash operation for a client (Slot::submission_accepted), the
// shared token must never be released on a bare application-level timeout --
// only a definitive NRF_EVT_FLASH_OPERATION_SUCCESS/ERROR event may release
// it. Releasing early would let InternalFS begin its own sd_flash_* call
// while ORUN's earlier operation is still genuinely in flight; the stale
// completion event that eventually arrives would then be misattributed (by
// the patched flash_nrf5x_event_cb(), which filters only by *current* shared
// owner, not by which physical request it actually belongs to) to
// InternalFS's new, unrelated, still-incomplete operation. A timed-out
// admitted request whose operation was already accepted is instead
// quarantined (Slot::quarantined): the caller is told kFailed so the
// application never wedges, but the shared token, in_flight_owner_ and the
// slot's kind/target stay exactly as they are until handleFlashEvent()
// observes the real completion and reconciles it (see quarantineSlot()).
// This also requires two independent timeout clocks, not one: how long an
// admitted request waits to ACQUIRE the token in the first place
// (kTokenWaitTimeoutMs) is a different question from how long it waits, once
// it actually owns the token, for its own submission/completion
// (kOperationTimeoutMs) -- see flash_mutation_gate.cpp for exactly what each
// one does and does not guarantee.
class FlashMutationGate : public FlashBackend {
 public:
  ~FlashMutationGate();

  // Lower value == admitted first when the physical slot is free and more
  // than one client has a staged, not-yet-admitted request. History and
  // Config always submit at their own fixed priority; Security's priority is
  // chosen per submission by which port (critical vs. maint) SecurityStore
  // used to make the call.
  enum class Priority : uint8_t { kSecCritical = 0, kHistory = 1, kConfig = 2, kSecMaint = 3 };

  struct Diagnostics {
    uint32_t submits = 0;
    uint32_t async_accepted = 0;
    uint32_t busy_retries = 0;
    uint32_t completions_success = 0;
    uint32_t completions_error = 0;
    uint32_t timeouts = 0;
    uint32_t spurious_events = 0;
    // A definitive completion event reconciled for a request that had
    // already been quarantined (timed out after SoftDevice accepted it, but
    // before a completion event arrived). Deliberately NOT counted in
    // completions_success/completions_error: the caller already observed
    // kFailed from the timeout and must not see a second, contradictory
    // outcome for the same logical request.
    uint32_t late_completions = 0;
  };

  // ---- History client: unchanged M7P3 FlashBackend API/behavior. ----
  bool begin() override;
  bool read(uint32_t offset, void* data, size_t size) const override;
  FlashOpResult program(uint32_t offset, const void* data, size_t size) override;
  FlashOpResult erasePage(uint32_t page) override;
  FlashOpResult pollPending() override;
  const Diagnostics& diagnostics() const { return history_diagnostics_; }

  // Before Bluefruit starts, drains SoftDevice SoC events directly via
  // sd_evt_get(), preserving the M7P3-M7P6 path. Once the patched Bluefruit
  // SoC task declares itself the queue owner, this method consumes only the
  // forwarded flash-event bridge and never drains sd_evt_get() itself.
  void pumpEvents();

  // ---- Config client (M7P5): symmetrical API, own region, own
  // diagnostics, sharing only the in-flight slot and pumpEvents() above.
  // Exposed as a plain FlashBackend view so ConfigStore holds a
  // FlashBackend&, exactly like HistoryStore does, unaware this is shared.
  class ConfigPort : public FlashBackend {
   public:
    explicit ConfigPort(FlashMutationGate& gate) : gate_(gate) {}
    bool begin() override { return gate_.beginConfig(); }
    bool read(uint32_t offset, void* data, size_t size) const override {
      return gate_.readConfig(offset, data, size);
    }
    FlashOpResult program(uint32_t offset, const void* data, size_t size) override {
      return gate_.programConfig(offset, data, size);
    }
    FlashOpResult erasePage(uint32_t page) override { return gate_.erasePageConfig(page); }
    FlashOpResult pollPending() override { return gate_.pollPendingConfig(); }

   private:
    FlashMutationGate& gate_;
  };
  FlashBackend& configPort() { return config_port_; }
  const Diagnostics& configDiagnostics() const { return config_diagnostics_; }

  // ---- Security client (M7P6B): two FlashBackend views of the SAME
  // underlying security client/slot/staging buffer, differing only in which
  // Priority they tag a submission with. SecurityStore itself decides which
  // view to call for a given internal step (see security_store.cpp); it
  // never has to know this is a shared gate. Both share one diagnostics
  // counter set (there is only ever one physical security request in flight
  // at a time, regardless of which port issued it).
  class SecurityCriticalPort : public FlashBackend {
   public:
    explicit SecurityCriticalPort(FlashMutationGate& gate) : gate_(gate) {}
    bool begin() override { return gate_.beginSecurity(); }
    bool read(uint32_t offset, void* data, size_t size) const override {
      return gate_.readSecurity(offset, data, size);
    }
    FlashOpResult program(uint32_t offset, const void* data, size_t size) override {
      return gate_.programSecurity(offset, data, size, Priority::kSecCritical);
    }
    FlashOpResult erasePage(uint32_t page) override {
      return gate_.erasePageSecurity(page, Priority::kSecCritical);
    }
    FlashOpResult pollPending() override { return gate_.pollPendingSecurity(); }

   private:
    FlashMutationGate& gate_;
  };
  class SecurityMaintPort : public FlashBackend {
   public:
    explicit SecurityMaintPort(FlashMutationGate& gate) : gate_(gate) {}
    bool begin() override { return gate_.beginSecurity(); }
    bool read(uint32_t offset, void* data, size_t size) const override {
      return gate_.readSecurity(offset, data, size);
    }
    FlashOpResult program(uint32_t offset, const void* data, size_t size) override {
      return gate_.programSecurity(offset, data, size, Priority::kSecMaint);
    }
    FlashOpResult erasePage(uint32_t page) override {
      return gate_.erasePageSecurity(page, Priority::kSecMaint);
    }
    FlashOpResult pollPending() override { return gate_.pollPendingSecurity(); }

   private:
    FlashMutationGate& gate_;
  };
  FlashBackend& securityCriticalPort() { return security_critical_port_; }
  FlashBackend& securityMaintPort() { return security_maint_port_; }
  const Diagnostics& securityDiagnostics() const { return security_diagnostics_; }

 private:
  friend class ConfigPort;
  friend class SecurityCriticalPort;
  friend class SecurityMaintPort;
  enum class Owner : uint8_t { kNone, kHistory, kConfig, kSecurity };
  enum class Kind : uint8_t { kNone, kProgram, kErase };

  struct Slot {
    Kind kind = Kind::kNone;
    uint32_t target = 0;  // program: absolute flash address; erase: absolute page index.
    // Fixed for History/Config (kHistory/kConfig); set per-submission for
    // Security depending on which port (critical/maint) was called.
    Priority priority = Priority::kConfig;
    // Base of the physical-operation timeout deadline. Meaningful only once
    // `admitted` is true -- a request may sit staged/queued behind another
    // client for an unbounded time (bounded only by that other client's own
    // admission+operation budget), and that queued wait must never count
    // against this request's own timeout.
    uint32_t started_ms = 0;
    // Set once, when this request is first staged (kind transitions from
    // kNone to Program/Erase) -- unlike `started_ms`, this runs from the
    // moment the caller asked, not from admission. Used only by
    // effectivePriority()'s aging check below; never affects the
    // post-admission physical-operation timeout.
    uint32_t staged_since_ms = 0;
    bool admitted = false;  // true once this request has actually obtained the physical in-flight slot.
    // True once this request has acquired the shared physical-flash token
    // (SharedFlashOwner::kGate) at least once. Set exactly once per
    // admission, the tick the token is acquired -- see attemptSubmit(). Used
    // only to know when to reset `started_ms` from "time since admission"
    // (bounded by kTokenWaitTimeoutMs while waiting for the token) to "time
    // since token acquisition" (bounded by kOperationTimeoutMs thereafter).
    bool token_acquired = false;
    bool submission_accepted = false;  // sd_flash_* itself returned NRF_SUCCESS.
    bool event_ready = false;          // pumpEvents() recorded a matching completion.
    bool event_success = false;
    // True once an admitted, already-accepted (submission_accepted) request
    // times out waiting for its completion event. The shared token is
    // deliberately NOT released while this is true -- see the class-level
    // "M7P7A ownership-transfer invariant" comment above and
    // quarantineSlot(). Only handleFlashEvent() observing the real,
    // definitive completion clears it (via releaseSlot()).
    bool quarantined = false;
    uint32_t staging_size = 0;
  };

  bool softDeviceEnabled() const;
  bool timedOut(uint32_t now, uint32_t started_ms) const;
  // A request staged (not yet admitted) for longer than kOperationTimeoutMs
  // -- the same bound already used for the post-admission physical-operation
  // timeout, not a new invented constant -- is treated as top priority
  // (kSecCritical) regardless of its real class. Without this, a lower
  // priority class (SEC_MAINT above all) could be starved indefinitely by
  // sustained higher-priority traffic that always has something staged the
  // instant the physical slot frees up; this bounds that wait instead of
  // requiring a generic fairness scheduler. Aging is per-request and
  // resets the instant that request is released (admitted, completed or
  // failed), so an aged-up request cannot itself cause more than roughly
  // one operation's worth of extra latency for the classes it now outranks.
  Priority effectivePriority(const Slot& slot) const;
  Slot& slot(Owner owner) {
    if (owner == Owner::kHistory) return history_slot_;
    if (owner == Owner::kConfig) return config_slot_;
    return security_slot_;
  }
  uint8_t* staging(Owner owner) {
    if (owner == Owner::kHistory) return history_staging_;
    if (owner == Owner::kConfig) return config_staging_;
    return security_staging_;
  }
  Diagnostics& diag(Owner owner) {
    if (owner == Owner::kHistory) return history_diagnostics_;
    if (owner == Owner::kConfig) return config_diagnostics_;
    return security_diagnostics_;
  }
  // True if some OTHER owner has a staged, not-yet-admitted request whose
  // priority strictly outranks `owner`'s own staged request. Generalizes the
  // old two-owner "config waits behind a staged history request" special
  // case to all three clients/five priority combinations (SEC_CRITICAL >
  // History > Config > SEC_MAINT) without changing History/Config's own
  // fixed relative order.
  bool higherPriorityWaiting(Owner owner) const;
  FlashOpResult attemptSubmit(Owner owner);
  FlashOpResult submitOrRetry(Owner owner);
  void releaseSlot(Owner owner);
  // Marks an already-accepted (submission_accepted), timed-out request as
  // quarantined. Deliberately does NOT touch the shared token,
  // in_flight_owner_, or any other slot field -- see the class-level
  // "M7P7A ownership-transfer invariant" comment.
  void quarantineSlot(Owner owner);
  void handleFlashEvent(uint32_t evt_id);

  bool beginConfig();
  bool readConfig(uint32_t offset, void* data, size_t size) const;
  FlashOpResult programConfig(uint32_t offset, const void* data, size_t size);
  FlashOpResult erasePageConfig(uint32_t page);
  FlashOpResult pollPendingConfig();

  bool beginSecurity();
  bool readSecurity(uint32_t offset, void* data, size_t size) const;
  FlashOpResult programSecurity(uint32_t offset, const void* data, size_t size, Priority priority);
  FlashOpResult erasePageSecurity(uint32_t page, Priority priority);
  FlashOpResult pollPendingSecurity();

  NrfHistoryFlash sync_history_;
  NrfConfigFlash sync_config_;
  NrfSecurityFlash sync_security_;
  bool ready_history_ = false;
  bool ready_config_ = false;
  bool ready_security_ = false;
  ConfigPort config_port_{*this};
  SecurityCriticalPort security_critical_port_{*this};
  SecurityMaintPort security_maint_port_{*this};
  Diagnostics history_diagnostics_{};
  Diagnostics config_diagnostics_{};
  Diagnostics security_diagnostics_{};

  // Shared: at most one physical Nordic flash mutation in flight at a time,
  // regardless of which client owns it.
  Owner in_flight_owner_ = Owner::kNone;
  // The most recent owner of in_flight_owner_ (set on admission, kept
  // across release). Used only to attribute a spurious event that arrives
  // with nobody currently in flight to whichever client most plausibly
  // caused it (its own just-completed/just-released operation), instead of
  // always crediting history by default.
  Owner last_owner_ = Owner::kHistory;
  Slot history_slot_{};
  Slot config_slot_{};
  Slot security_slot_{};
  // Owned staging copies: Nordic's own documentation requires the source
  // buffer to remain unmodified until the completion event arrives when
  // SoftDevice is enabled, so this backend never keeps a pointer into
  // caller-owned memory across a kPending boundary. Sized for each client's
  // own largest blob -- history's storage_config::kPageHeaderSize (the same
  // bound HistoryStore itself uses for its own blob_ buffer), config's and
  // security's small fixed caps (real maxes are config_format::kRecordSize,
  // 36 bytes, and security_format::kCredentialRecordSize, 68 bytes; this
  // file does not depend on either format header to stay decoupled).
  alignas(4) uint8_t history_staging_[storage_config::kPageHeaderSize]{};
  alignas(4) uint8_t config_staging_[64]{};
  alignas(4) uint8_t security_staging_[96]{};
};

}  // namespace orun_tlp
