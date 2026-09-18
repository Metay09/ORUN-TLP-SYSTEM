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
// No SecurityStore/Bond client is added or queued here; those remain
// unimplemented (M7P6) and M7P4 already documented that relocated
// InternalFS bypasses this gate entirely (a real, separately-tracked M7P7
// prerequisite, not solved by this generalization).
class FlashMutationGate : public FlashBackend {
 public:
  struct Diagnostics {
    uint32_t submits = 0;
    uint32_t async_accepted = 0;
    uint32_t busy_retries = 0;
    uint32_t completions_success = 0;
    uint32_t completions_error = 0;
    uint32_t timeouts = 0;
    uint32_t spurious_events = 0;
  };

  // ---- History client: unchanged M7P3 FlashBackend API/behavior. ----
  bool begin() override;
  bool read(uint32_t offset, void* data, size_t size) const override;
  FlashOpResult program(uint32_t offset, const void* data, size_t size) override;
  FlashOpResult erasePage(uint32_t page) override;
  FlashOpResult pollPending() override;
  const Diagnostics& diagnostics() const { return history_diagnostics_; }

  // Drains NRF_EVT_FLASH_OPERATION_SUCCESS/ERROR (and discards any other
  // pending SoC event) from the SoftDevice event queue via the raw sd_evt_get
  // SVC, for BOTH clients -- the single global queue has exactly one drain
  // point now, routed to whichever client currently owns the in-flight
  // operation. Must be called once per cooperative loop pass. A no-op
  // whenever SoftDevice is disabled -- in shipped firmware today, always.
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

 private:
  friend class ConfigPort;
  enum class Owner : uint8_t { kNone, kHistory, kConfig };
  enum class Kind : uint8_t { kNone, kProgram, kErase };

  struct Slot {
    Kind kind = Kind::kNone;
    uint32_t target = 0;  // program: absolute flash address; erase: absolute page index.
    // Base of the physical-operation timeout deadline. Meaningful only once
    // `admitted` is true -- a request may sit staged/queued behind the
    // other client for an unbounded time (bounded only by that other
    // client's own admission+operation budget), and that queued wait must
    // never count against this request's own timeout.
    uint32_t started_ms = 0;
    bool admitted = false;  // true once this request has actually obtained the physical in-flight slot.
    bool submission_accepted = false;  // sd_flash_* itself returned NRF_SUCCESS.
    bool event_ready = false;          // pumpEvents() recorded a matching completion.
    bool event_success = false;
    uint32_t staging_size = 0;
  };

  bool softDeviceEnabled() const;
  bool timedOut(uint32_t now, uint32_t started_ms) const;
  Slot& slot(Owner owner) { return owner == Owner::kHistory ? history_slot_ : config_slot_; }
  uint8_t* staging(Owner owner) { return owner == Owner::kHistory ? history_staging_ : config_staging_; }
  Diagnostics& diag(Owner owner) { return owner == Owner::kHistory ? history_diagnostics_ : config_diagnostics_; }
  FlashOpResult attemptSubmit(Owner owner);
  FlashOpResult submitOrRetry(Owner owner);
  void releaseSlot(Owner owner);

  bool beginConfig();
  bool readConfig(uint32_t offset, void* data, size_t size) const;
  FlashOpResult programConfig(uint32_t offset, const void* data, size_t size);
  FlashOpResult erasePageConfig(uint32_t page);
  FlashOpResult pollPendingConfig();

  NrfHistoryFlash sync_history_;
  NrfConfigFlash sync_config_;
  bool ready_history_ = false;
  bool ready_config_ = false;
  ConfigPort config_port_{*this};
  Diagnostics history_diagnostics_{};
  Diagnostics config_diagnostics_{};

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
  // Owned staging copies: Nordic's own documentation requires the source
  // buffer to remain unmodified until the completion event arrives when
  // SoftDevice is enabled, so this backend never keeps a pointer into
  // caller-owned memory across a kPending boundary. Sized for each client's
  // own largest blob -- history's storage_config::kPageHeaderSize (the same
  // bound HistoryStore itself uses for its own blob_ buffer), config's a
  // small fixed cap (its real max is config_format::kRecordSize, 36 bytes;
  // this file does not depend on config_format.h to stay decoupled).
  alignas(4) uint8_t history_staging_[storage_config::kPageHeaderSize]{};
  alignas(4) uint8_t config_staging_[64]{};
};

}  // namespace orun_tlp
