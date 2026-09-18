#include "flash_mutation_gate.h"

#include <string.h>
#include <nrf_sdm.h>
#include <nrf_soc.h>

#include "monotonic_time.h"

namespace orun_tlp {
namespace {
using namespace storage_config;

// Nordic's own S140 6.1.1 sd_flash_write/sd_flash_page_erase documentation
// ("nrf_soc.h", read from the installed framework, not from memory) states
// completion is communicated by exactly one of NRF_EVT_FLASH_OPERATION_SUCCESS
// or NRF_EVT_FLASH_OPERATION_ERROR once SoftDevice is enabled, and that a
// submission may return NRF_ERROR_BUSY if the previous command has not yet
// completed. The framework's own InternalFS backend
// (libraries/InternalFileSytem/src/flash/flash_nrf5x.c, fal_erase/
// fal_sub_program) retries up to 20 times with a short delay between
// attempts on any non-success return. This gate applies the same bounded,
// non-blocking retry *philosophy* (never sleeps/spins; a BUSY result is
// reported as kPending and retried on a later pollPending() call from
// normal loop context) but bounds it by the wall-clock timeout below, not a
// fixed attempt count -- a slow poll cadence would exhaust a fixed count
// before genuinely giving up, where the timeout scales with real elapsed
// time regardless of how often pollPending() happens to be called.
//
// This budget is scoped to Slot::admitted, i.e. to time actually spent
// holding the shared physical in-flight slot -- not to time a request spent
// staged and queued behind the other client's own operation (which the
// M7P5 dual-client admission queue can now make arbitrarily long, bounded
// only by the other client's own admission+operation time, itself bounded
// by this same constant). See submitOrRetry()'s admission block.
constexpr uint32_t kOperationTimeoutMs = 4000;

bool inBoundsHistory(uint32_t offset, size_t size) {
  return offset <= kRegionSize && size <= kRegionSize - offset;
}
constexpr uint32_t kConfigRegionSize = kFutureConfigRegionEnd - kFutureConfigRegionStart;
bool inBoundsConfig(uint32_t offset, size_t size) {
  return offset <= kConfigRegionSize && size <= kConfigRegionSize - offset;
}
constexpr uint32_t kSecurityRegionSize = kFutureSecurityRegionEnd - kFutureSecurityRegionStart;
bool inBoundsSecurity(uint32_t offset, size_t size) {
  return offset <= kSecurityRegionSize && size <= kSecurityRegionSize - offset;
}
}  // namespace

// ---------------------------------------------------------------------
// History client: unchanged M7P3 FlashBackend surface and behavior.
// ---------------------------------------------------------------------

bool FlashMutationGate::begin() {
  ready_history_ = sync_history_.begin();
  return ready_history_;
}

bool FlashMutationGate::read(uint32_t offset, void* data, size_t size) const {
  return sync_history_.read(offset, data, size);
}

bool FlashMutationGate::softDeviceEnabled() const {
  uint8_t enabled = 1;
  return sd_softdevice_is_enabled(&enabled) == NRF_SUCCESS && enabled != 0;
}

bool FlashMutationGate::timedOut(uint32_t now, uint32_t started_ms) const {
  return monotonic::elapsed(now, started_ms, kOperationTimeoutMs);
}

void FlashMutationGate::releaseSlot(Owner owner) {
  Slot& mine = slot(owner);
  mine.kind = Kind::kNone;
  mine.admitted = false;
  mine.submission_accepted = false;
  mine.event_ready = false;
  mine.staging_size = 0;
  if (in_flight_owner_ == owner) in_flight_owner_ = Owner::kNone;
}

FlashOpResult FlashMutationGate::program(uint32_t offset, const void* data, size_t size) {
  if (!ready_history_) return FlashOpResult::kFailed;
  ++history_diagnostics_.submits;
  // Deliberately re-checks SoftDevice state here even though
  // sync_history_.program() performs its own independent
  // synchronousFlashAvailable() check when this delegates to it: two
  // independent guards checking the same fact is intentional defense in
  // depth, not redundancy to remove -- NrfHistoryFlash's own guard must stay
  // intact unweakened (see its class comment), and this gate's guard is what
  // decides which of the two entirely different code paths (sync delegate
  // vs. raw async SVCs) to take in the first place.
  if (!softDeviceEnabled()) return sync_history_.program(offset, data, size);

  // Async path: validate exactly as the synchronous backend does (same
  // storage_config bounds, alignment, and erased-destination precondition),
  // then stage an owned copy before issuing the request -- the source
  // buffer's lifetime is not guaranteed by the caller past this call.
  if (history_slot_.kind != Kind::kNone) return FlashOpResult::kFailed;
  if (!data || !size || !inBoundsHistory(offset, size) || (offset & 3U) != 0 ||
      (size & 3U) != 0 || size > sizeof(history_staging_) ||
      size > kPageSize - offset % kPageSize)
    return FlashOpResult::kFailed;
  const auto* destination = reinterpret_cast<const uint8_t*>(kBaseAddress + offset);
  for (size_t index = 0; index < size; ++index)
    if (destination[index] != 0xFF) return FlashOpResult::kFailed;

  memcpy(history_staging_, data, size);
  history_slot_.staging_size = size;
  history_slot_.kind = Kind::kProgram;
  history_slot_.target = kBaseAddress + offset;
  history_slot_.priority = Priority::kHistory;
  history_slot_.staged_since_ms = monotonic::nowMs();
  // started_ms is set on admission (submitOrRetry), not here -- this request
  // may still have to wait behind the other client's in-flight operation.
  history_slot_.admitted = false;
  history_slot_.submission_accepted = false;
  history_slot_.event_ready = false;
  return submitOrRetry(Owner::kHistory);
}

FlashOpResult FlashMutationGate::erasePage(uint32_t page) {
  if (!ready_history_) return FlashOpResult::kFailed;
  ++history_diagnostics_.submits;
  if (!softDeviceEnabled()) return sync_history_.erasePage(page);

  if (history_slot_.kind != Kind::kNone) return FlashOpResult::kFailed;
  if (page >= kPageCount) return FlashOpResult::kFailed;

  history_slot_.kind = Kind::kErase;
  history_slot_.target = kBaseAddress / kPageSize + page;
  history_slot_.priority = Priority::kHistory;
  history_slot_.staged_since_ms = monotonic::nowMs();
  history_slot_.admitted = false;
  history_slot_.submission_accepted = false;
  history_slot_.event_ready = false;
  return submitOrRetry(Owner::kHistory);
}

FlashOpResult FlashMutationGate::pollPending() {
  if (history_slot_.kind == Kind::kNone) return FlashOpResult::kFailed;
  return submitOrRetry(Owner::kHistory);
}

// ---------------------------------------------------------------------
// Config client (M7P5): symmetrical API, own region, own diagnostics.
// ---------------------------------------------------------------------

bool FlashMutationGate::beginConfig() {
  ready_config_ = sync_config_.begin();
  return ready_config_;
}

bool FlashMutationGate::readConfig(uint32_t offset, void* data, size_t size) const {
  return sync_config_.read(offset, data, size);
}

FlashOpResult FlashMutationGate::programConfig(uint32_t offset, const void* data, size_t size) {
  if (!ready_config_) return FlashOpResult::kFailed;
  ++config_diagnostics_.submits;
  if (!softDeviceEnabled()) return sync_config_.program(offset, data, size);

  if (config_slot_.kind != Kind::kNone) return FlashOpResult::kFailed;
  if (!data || !size || !inBoundsConfig(offset, size) || (offset & 3U) != 0 ||
      (size & 3U) != 0 || size > sizeof(config_staging_) ||
      size > kPageSize - offset % kPageSize)
    return FlashOpResult::kFailed;
  const auto* destination =
      reinterpret_cast<const uint8_t*>(kFutureConfigRegionStart + offset);
  for (size_t index = 0; index < size; ++index)
    if (destination[index] != 0xFF) return FlashOpResult::kFailed;

  memcpy(config_staging_, data, size);
  config_slot_.staging_size = size;
  config_slot_.kind = Kind::kProgram;
  config_slot_.target = kFutureConfigRegionStart + offset;
  config_slot_.priority = Priority::kConfig;
  config_slot_.staged_since_ms = monotonic::nowMs();
  // started_ms is set on admission (submitOrRetry), not here -- this request
  // may still have to wait behind the other client's in-flight operation.
  config_slot_.admitted = false;
  config_slot_.submission_accepted = false;
  config_slot_.event_ready = false;
  return submitOrRetry(Owner::kConfig);
}

FlashOpResult FlashMutationGate::erasePageConfig(uint32_t page) {
  if (!ready_config_) return FlashOpResult::kFailed;
  ++config_diagnostics_.submits;
  if (!softDeviceEnabled()) return sync_config_.erasePage(page);

  if (config_slot_.kind != Kind::kNone) return FlashOpResult::kFailed;
  if (page >= kFutureConfigRegionPages) return FlashOpResult::kFailed;

  config_slot_.kind = Kind::kErase;
  config_slot_.target = kFutureConfigRegionStart / kPageSize + page;
  config_slot_.priority = Priority::kConfig;
  config_slot_.staged_since_ms = monotonic::nowMs();
  config_slot_.admitted = false;
  config_slot_.submission_accepted = false;
  config_slot_.event_ready = false;
  return submitOrRetry(Owner::kConfig);
}

FlashOpResult FlashMutationGate::pollPendingConfig() {
  if (config_slot_.kind == Kind::kNone) return FlashOpResult::kFailed;
  return submitOrRetry(Owner::kConfig);
}

// ---------------------------------------------------------------------
// Security client (M7P6B): symmetrical API, own region, own diagnostics,
// two priority-tagged entry points (SecurityCriticalPort/SecurityMaintPort)
// sharing one physical slot -- SecurityStore only ever has one request
// outstanding at a time regardless of which port it used.
// ---------------------------------------------------------------------

bool FlashMutationGate::beginSecurity() {
  ready_security_ = sync_security_.begin();
  return ready_security_;
}

bool FlashMutationGate::readSecurity(uint32_t offset, void* data, size_t size) const {
  return sync_security_.read(offset, data, size);
}

FlashOpResult FlashMutationGate::programSecurity(uint32_t offset, const void* data, size_t size,
                                                 Priority priority) {
  if (!ready_security_) return FlashOpResult::kFailed;
  ++security_diagnostics_.submits;
  if (!softDeviceEnabled()) return sync_security_.program(offset, data, size);

  if (security_slot_.kind != Kind::kNone) return FlashOpResult::kFailed;
  if (!data || !size || !inBoundsSecurity(offset, size) || (offset & 3U) != 0 ||
      (size & 3U) != 0 || size > sizeof(security_staging_) ||
      size > kPageSize - offset % kPageSize)
    return FlashOpResult::kFailed;
  const auto* destination =
      reinterpret_cast<const uint8_t*>(kFutureSecurityRegionStart + offset);
  for (size_t index = 0; index < size; ++index)
    if (destination[index] != 0xFF) return FlashOpResult::kFailed;

  memcpy(security_staging_, data, size);
  security_slot_.staging_size = size;
  security_slot_.kind = Kind::kProgram;
  security_slot_.target = kFutureSecurityRegionStart + offset;
  security_slot_.priority = priority;
  security_slot_.staged_since_ms = monotonic::nowMs();
  security_slot_.admitted = false;
  security_slot_.submission_accepted = false;
  security_slot_.event_ready = false;
  return submitOrRetry(Owner::kSecurity);
}

FlashOpResult FlashMutationGate::erasePageSecurity(uint32_t page, Priority priority) {
  if (!ready_security_) return FlashOpResult::kFailed;
  ++security_diagnostics_.submits;
  if (!softDeviceEnabled()) return sync_security_.erasePage(page);

  if (security_slot_.kind != Kind::kNone) return FlashOpResult::kFailed;
  if (page >= kFutureSecurityRegionPages) return FlashOpResult::kFailed;

  security_slot_.kind = Kind::kErase;
  security_slot_.target = kFutureSecurityRegionStart / kPageSize + page;
  security_slot_.priority = priority;
  security_slot_.staged_since_ms = monotonic::nowMs();
  security_slot_.admitted = false;
  security_slot_.submission_accepted = false;
  security_slot_.event_ready = false;
  return submitOrRetry(Owner::kSecurity);
}

FlashOpResult FlashMutationGate::pollPendingSecurity() {
  if (security_slot_.kind == Kind::kNone) return FlashOpResult::kFailed;
  return submitOrRetry(Owner::kSecurity);
}

// ---------------------------------------------------------------------
// Shared admission, submission, and event routing.
// ---------------------------------------------------------------------

// A request staged (kind != kNone, not yet admitted) for longer than
// kOperationTimeoutMs is treated as top priority regardless of its real
// class -- see the declaration comment (flash_mutation_gate.h) for why this
// bounded aging exists: without it, SEC_MAINT (or any lower class) could be
// starved indefinitely by sustained higher-priority traffic that always has
// something staged the instant the physical slot frees up.
FlashMutationGate::Priority FlashMutationGate::effectivePriority(const Slot& slot) const {
  if (slot.kind != Kind::kNone && !slot.admitted &&
      monotonic::elapsed(monotonic::nowMs(), slot.staged_since_ms, kOperationTimeoutMs)) {
    return Priority::kSecCritical;
  }
  return slot.priority;
}

bool FlashMutationGate::higherPriorityWaiting(Owner owner) const {
  const Slot* mine = owner == Owner::kHistory ? &history_slot_
                    : owner == Owner::kConfig  ? &config_slot_
                                                : &security_slot_;
  const Priority mine_priority = effectivePriority(*mine);
  const Slot* others[] = {&history_slot_, &config_slot_, &security_slot_};
  const Owner owners[] = {Owner::kHistory, Owner::kConfig, Owner::kSecurity};
  for (unsigned index = 0; index < 3; ++index) {
    if (owners[index] == owner) continue;
    const Slot& other = *others[index];
    if (other.kind != Kind::kNone && !other.admitted &&
        effectivePriority(other) < mine_priority)
      return true;
  }
  return false;
}

// Bounded, no-heap admission: the physical in-flight slot is free, or
// already owned by `owner` (a retry), or owned by another client (this call
// stays queued). Priority (ADR §7.1/§10: SEC_CRITICAL > History > Config >
// SEC_MAINT) is enforced here, independent of which client happens to call
// first: if the slot is free but a higher-priority client currently has a
// request staged and not yet admitted, this owner is held back so that
// client is admitted next, not whichever client asked first. A client that
// already owns the physical slot cannot be preempted -- the Nordic SVCs
// have no cancel -- so this governs admission order only, never
// interruption of an already-accepted operation.
FlashOpResult FlashMutationGate::submitOrRetry(Owner owner) {
  if (in_flight_owner_ == Owner::kNone) {
    if (higherPriorityWaiting(owner)) return FlashOpResult::kPending;
    in_flight_owner_ = last_owner_ = owner;
  }
  if (in_flight_owner_ != owner) return FlashOpResult::kPending;  // Queued behind the other owner.

  Slot& mine = slot(owner);
  if (!mine.admitted) {
    // The bounded physical-operation timeout below must measure time this
    // request actually spent holding the in-flight slot, never time it
    // spent merely staged/queued behind the other client's own operation.
    // A request that waited >kOperationTimeoutMs in the admission queue and
    // is only now admitted still gets a full, fresh budget starting now --
    // otherwise it could be failed closed on the very tick sd_flash_* is
    // first attempted, before it ever had a real chance to complete.
    mine.admitted = true;
    mine.started_ms = monotonic::nowMs();
  }
  if (!mine.submission_accepted) return attemptSubmit(owner);
  if (mine.event_ready) {
    const bool ok = mine.event_success;
    releaseSlot(owner);
    if (ok) {
      ++diag(owner).completions_success;
      return FlashOpResult::kDone;
    }
    ++diag(owner).completions_error;
    return FlashOpResult::kFailed;
  }
  if (timedOut(monotonic::nowMs(), mine.started_ms)) {
    // A lost/missing completion event must not wedge the device: fail this
    // request closed at the application level. This is not a claim that the
    // physical write/erase did or did not happen -- the flash backend never
    // asserts durability from a timeout, only from a confirmed SUCCESS
    // event or, when SoftDevice is disabled, an immediate verified readback.
    releaseSlot(owner);
    ++diag(owner).timeouts;
    return FlashOpResult::kFailed;
  }
  return FlashOpResult::kPending;
}

FlashOpResult FlashMutationGate::attemptSubmit(Owner owner) {
  Slot& mine = slot(owner);
  uint32_t result = NRF_ERROR_INTERNAL;
  if (mine.kind == Kind::kProgram) {
    result = sd_flash_write(reinterpret_cast<uint32_t*>(mine.target),
                            reinterpret_cast<const uint32_t*>(staging(owner)),
                            mine.staging_size / sizeof(uint32_t));
  } else if (mine.kind == Kind::kErase) {
    result = sd_flash_page_erase(mine.target);
  }

  if (result == NRF_SUCCESS) {
    mine.submission_accepted = true;
    ++diag(owner).async_accepted;
    return FlashOpResult::kPending;
  }
  if (result == NRF_ERROR_BUSY) {
    ++diag(owner).busy_retries;
    if (timedOut(monotonic::nowMs(), mine.started_ms)) {
      releaseSlot(owner);
      ++diag(owner).timeouts;
      return FlashOpResult::kFailed;
    }
    return FlashOpResult::kPending;  // Retry submission from pollPending().
  }
  // Permanent rejection (invalid address/length, forbidden region, or an
  // internal SoftDevice error opening the session): fail closed now rather
  // than retrying a request the SoftDevice has already refused to start.
  releaseSlot(owner);
  return FlashOpResult::kFailed;
}

void FlashMutationGate::pumpEvents() {
  if (!softDeviceEnabled()) return;
  uint32_t evt_id = 0;
  while (sd_evt_get(&evt_id) == NRF_SUCCESS) {
    if (evt_id != NRF_EVT_FLASH_OPERATION_SUCCESS &&
        evt_id != NRF_EVT_FLASH_OPERATION_ERROR) {
      // Not a flash event (e.g. HFCLKSTARTED, POWER_*, RADIO_* timeslot,
      // USB_*): drained so the SoftDevice event queue never backs up, but
      // otherwise not acted on by this slice.
      continue;
    }
    if (in_flight_owner_ == Owner::kNone) {
      // No client currently owns the physical slot at all: attribute this
      // to whichever client most recently did (its own just-completed or
      // just-released operation is the most plausible source of a stray
      // late event), rather than always crediting history by default.
      ++diag(last_owner_).spurious_events;
      continue;
    }
    Slot& mine = slot(in_flight_owner_);
    if (!mine.submission_accepted || mine.event_ready) {
      // The owning client hasn't actually had its submission accepted yet
      // (still BUSY-retrying), or a second flash event arrived before
      // pollPending() consumed the first (the SoftDevice API documents
      // exactly one event per command, so this should not happen) --
      // discard rather than completing the wrong request or double-calling
      // completion. Routed to the current owner's own counter: only that
      // owner could plausibly have caused a spurious event right now, since
      // it is the only client with a physical slot.
      ++diag(in_flight_owner_).spurious_events;
      continue;
    }
    mine.event_ready = true;
    mine.event_success = (evt_id == NRF_EVT_FLASH_OPERATION_SUCCESS);
  }
}

}  // namespace orun_tlp
