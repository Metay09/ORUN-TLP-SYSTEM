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

// M7P7A: independent from kOperationTimeoutMs above. Bounds only how long an
// admitted request waits to ACQUIRE the shared physical-flash token in the
// first place -- before this request has ever had a chance to call
// sd_flash_write/sd_flash_page_erase itself. Reusing kOperationTimeoutMs
// (4000ms) for this wait was the review-found defect: a queued ORUN request
// could fail closed before it ever had one opportunity to submit anything to
// SoftDevice, purely because it shared a clock with the unrelated
// post-acquisition phase below.
//
// patch_ble_flash.py's own ORUN_FLASH_ARBITER_WAIT_MS (4500ms) bounds the
// mirror-image wait -- how long InternalFS's patched driver waits to ACQUIRE
// this same token while ORUN owns it. The two constants are each one side's
// own "how long will I wait to acquire" policy; they do not bound the same
// phase from opposite directions, and one is not a correctness precondition
// for the other. In particular, ORUN_FLASH_ARBITER_WAIT_MS does NOT bound how
// long InternalFS then HOLDS the token after it acquires it: once its
// sd_flash_write/sd_flash_page_erase call is accepted, the stock (unpatched)
// wait_for_async_flash_op_completion() blocks on xSemaphoreTake(_sem,
// portMAX_DELAY) -- an unbounded wait for the real completion event, with no
// software timeout of its own. So "kTokenWaitTimeoutMs > ORUN_FLASH_ARBITER_
// WAIT_MS" is not, and was never meant to be, a proof that ORUN is guaranteed
// to obtain the token within kTokenWaitTimeoutMs.
//
// kTokenWaitTimeoutMs is instead ORUN's own explicit, independent bounded
// liveness policy: if InternalFS still owns the token when this fires (for
// any reason -- still waiting to acquire it, or already holding it and
// waiting on its own completion event), this admitted request fails closed.
// This is always safe from an ownership-ambiguity standpoint: nothing was
// ever submitted to SoftDevice by ORUN in this branch (submission_accepted is
// still false), so releasing/not-touching the token here can never race a
// still-outstanding ORUN operation the way an accepted-operation timeout
// could (see the class-level "M7P7A ownership-transfer invariant" comment in
// flash_mutation_gate.h, and quarantineSlot() below) -- it only ever means
// "InternalFS still owns it; I am done waiting." A permanently stuck
// InternalFS/SoftDevice path (the portMAX_DELAY case truly never completing)
// is a real liveness gap this constant does not solve; it is a physical BLE
// runtime concern for later validation, not an ownership-safety one.
constexpr uint32_t kTokenWaitTimeoutMs = 6000;

// M7P7A cross-task physical-flash bridge. The cooperative ORUN gate and
// Adafruit InternalFS may run from different FreeRTOS tasks once BLE starts.
// Keep only two machine-word atomics here: one owner token and one forwarded
// gate-event mailbox. FlashMutationGate object state itself remains owned by
// the normal cooperative loop.
enum class SharedFlashOwner : uint32_t {
  kNone = 0,
  kGate = 1,
  kInternalFs = 2,
};

volatile uint32_t g_shared_flash_owner =
    static_cast<uint32_t>(SharedFlashOwner::kNone);
volatile uint32_t g_bluefruit_soc_event_owner = 0;
volatile uint32_t g_gate_flash_event = 0;  // 0 none, 1 success, 2 error.

uint32_t loadSharedOwner() {
  return __atomic_load_n(&g_shared_flash_owner, __ATOMIC_ACQUIRE);
}

bool sharedOwnerIs(SharedFlashOwner owner) {
  return loadSharedOwner() == static_cast<uint32_t>(owner);
}

bool tryAcquireSharedFlash(SharedFlashOwner owner) {
  uint32_t expected = static_cast<uint32_t>(SharedFlashOwner::kNone);
  return __atomic_compare_exchange_n(
      &g_shared_flash_owner, &expected, static_cast<uint32_t>(owner), false,
      __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

void releaseSharedFlash(SharedFlashOwner owner) {
  uint32_t expected = static_cast<uint32_t>(owner);
  (void)__atomic_compare_exchange_n(
      &g_shared_flash_owner, &expected,
      static_cast<uint32_t>(SharedFlashOwner::kNone), false,
      __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

bool bluefruitOwnsSocEvents() {
  return __atomic_load_n(&g_bluefruit_soc_event_owner, __ATOMIC_ACQUIRE) != 0;
}

uint32_t takeBridgedGateEvent() {
  return __atomic_exchange_n(&g_gate_flash_event, 0U, __ATOMIC_ACQ_REL);
}

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

}  // namespace orun_tlp

// Weak references to these C-linkage hooks are injected into the pinned
// Adafruit framework by M7P7A. Keeping the strong implementation here makes
// FlashMutationGate the application owner of arbitration without teaching the
// vendor C/C++ sources about ORUN types.
extern "C" bool orun_flash_internalfs_try_acquire(void) {
  return orun_tlp::tryAcquireSharedFlash(
      orun_tlp::SharedFlashOwner::kInternalFs);
}

extern "C" void orun_flash_internalfs_release(void) {
  orun_tlp::releaseSharedFlash(orun_tlp::SharedFlashOwner::kInternalFs);
}

extern "C" bool orun_flash_internalfs_owns(void) {
  return orun_tlp::sharedOwnerIs(orun_tlp::SharedFlashOwner::kInternalFs);
}

extern "C" void orun_flash_gate_soc_event_cb(uint32_t event) {
  if (!orun_tlp::sharedOwnerIs(orun_tlp::SharedFlashOwner::kGate)) return;

  uint32_t encoded = 0;
  if (event == NRF_EVT_FLASH_OPERATION_SUCCESS) encoded = 1;
  else if (event == NRF_EVT_FLASH_OPERATION_ERROR) encoded = 2;
  if (encoded != 0) {
    __atomic_store_n(&orun_tlp::g_gate_flash_event, encoded, __ATOMIC_RELEASE);
  }
}

extern "C" void orun_flash_gate_set_bluefruit_soc_owner(bool active) {
  __atomic_store_n(&orun_tlp::g_bluefruit_soc_event_owner,
                   active ? 1U : 0U, __ATOMIC_RELEASE);
}

namespace orun_tlp {

// ---------------------------------------------------------------------
// History client: unchanged M7P3 FlashBackend surface and behavior.
// ---------------------------------------------------------------------

FlashMutationGate::~FlashMutationGate() {
  // The product has one process-lifetime gate. Cleanup mainly isolates scoped
  // host-test instances so one interrupted scenario cannot poison the next.
  releaseSharedFlash(SharedFlashOwner::kGate);
  __atomic_store_n(&g_gate_flash_event, 0U, __ATOMIC_RELEASE);
  __atomic_store_n(&g_bluefruit_soc_event_owner, 0U, __ATOMIC_RELEASE);
}

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
  mine.token_acquired = false;
  mine.submission_accepted = false;
  mine.event_ready = false;
  mine.quarantined = false;
  mine.staging_size = 0;
  if (in_flight_owner_ == owner) in_flight_owner_ = Owner::kNone;
  // Keep the global token across SoftDevice BUSY retries, but never beyond
  // completion/failure of this admitted ORUN request.
  releaseSharedFlash(SharedFlashOwner::kGate);
}

void FlashMutationGate::quarantineSlot(Owner owner) {
  // Deliberately the ONLY field touched: kind/target/admitted/
  // submission_accepted, in_flight_owner_ and the shared kGate token all
  // stay exactly as they are. A genuinely late handleFlashEvent() must still
  // find in_flight_owner_ == owner to reconcile this exact request, and a
  // new call from `owner` must see kind != kNone and be rejected until that
  // reconciliation happens -- see the class-level "M7P7A ownership-transfer
  // invariant" comment in flash_mutation_gate.h.
  slot(owner).quarantined = true;
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
  history_slot_.token_acquired = false;
  history_slot_.submission_accepted = false;
  history_slot_.event_ready = false;
  history_slot_.quarantined = false;
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
  history_slot_.token_acquired = false;
  history_slot_.submission_accepted = false;
  history_slot_.event_ready = false;
  history_slot_.quarantined = false;
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
  config_slot_.token_acquired = false;
  config_slot_.submission_accepted = false;
  config_slot_.event_ready = false;
  config_slot_.quarantined = false;
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
  config_slot_.token_acquired = false;
  config_slot_.submission_accepted = false;
  config_slot_.event_ready = false;
  config_slot_.quarantined = false;
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
  security_slot_.token_acquired = false;
  security_slot_.submission_accepted = false;
  security_slot_.event_ready = false;
  security_slot_.quarantined = false;
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
  security_slot_.token_acquired = false;
  security_slot_.submission_accepted = false;
  security_slot_.event_ready = false;
  security_slot_.quarantined = false;
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
    // SoftDevice already accepted this operation (submission_accepted,
    // checked above) but no completion event has arrived. This must NOT
    // release the shared token -- see the class-level "M7P7A
    // ownership-transfer invariant" comment in flash_mutation_gate.h and
    // quarantineSlot(). The application still gets a bounded kFailed so it
    // never wedges; this is not a claim that the physical write/erase did or
    // did not happen -- the flash backend never asserts durability from a
    // timeout, only from a confirmed SUCCESS event or, when SoftDevice is
    // disabled, an immediate verified readback.
    quarantineSlot(owner);
    ++diag(owner).timeouts;
    return FlashOpResult::kFailed;
  }
  return FlashOpResult::kPending;
}

FlashOpResult FlashMutationGate::attemptSubmit(Owner owner) {
  Slot& mine = slot(owner);

  // Phase 1: wait for the shared physical-flash token, currently held by
  // InternalFS. Bounded by kTokenWaitTimeoutMs, deliberately NOT
  // kOperationTimeoutMs -- see kTokenWaitTimeoutMs's declaration comment.
  // Once an admitted ORUN request acquires the token it retains it across
  // NRF_ERROR_BUSY retries; this prevents a bond write from interleaving
  // between retries of a security/history/config operation.
  if (!sharedOwnerIs(SharedFlashOwner::kGate) &&
      !tryAcquireSharedFlash(SharedFlashOwner::kGate)) {
    ++diag(owner).busy_retries;
    if (monotonic::elapsed(monotonic::nowMs(), mine.started_ms, kTokenWaitTimeoutMs)) {
      // This branch only runs while InternalFS (not this gate) owns the
      // token, so releaseSlot()'s releaseSharedFlash(kGate) call below is a
      // harmless no-op here (its compare-exchange expects the current owner
      // to already be kGate) -- it resets this request's own local admission
      // bookkeeping without touching InternalFS's actual ownership. Nothing
      // was ever submitted to SoftDevice for this request, so failing closed
      // here can never create ownership ambiguity, no matter why InternalFS
      // still owned the token (see kTokenWaitTimeoutMs's declaration
      // comment).
      releaseSlot(owner);
      ++diag(owner).timeouts;
      return FlashOpResult::kFailed;
    }
    return FlashOpResult::kPending;
  }

  // Token acquired -- either just now, or retained from an earlier retry of
  // this same admitted request. Give the post-acquisition phase (SD BUSY
  // retries on the submission call itself, then submitOrRetry()'s wait for
  // the completion event) its own fresh kOperationTimeoutMs budget,
  // independent of how long phase 1 above took.
  if (!mine.token_acquired) {
    mine.token_acquired = true;
    mine.started_ms = monotonic::nowMs();
  }

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
    return FlashOpResult::kPending;
  }

  // Permanent rejection: fail closed and release both the ORUN admission
  // slot and the shared physical-flash token.
  releaseSlot(owner);
  return FlashOpResult::kFailed;
}

void FlashMutationGate::handleFlashEvent(uint32_t evt_id) {
  if (evt_id != NRF_EVT_FLASH_OPERATION_SUCCESS &&
      evt_id != NRF_EVT_FLASH_OPERATION_ERROR)
    return;

  if (in_flight_owner_ == Owner::kNone) {
    ++diag(last_owner_).spurious_events;
    return;
  }

  Slot& mine = slot(in_flight_owner_);
  if (!mine.submission_accepted) {
    ++diag(in_flight_owner_).spurious_events;
    return;
  }

  if (mine.quarantined) {
    // A definitive completion for a request whose application-level caller
    // already timed out (submitOrRetry() already returned kFailed). Only
    // now -- never on a bare timeout -- is it safe to release the shared
    // token: see the class-level "M7P7A ownership-transfer invariant"
    // comment in flash_mutation_gate.h. Deliberately does not set
    // event_ready/event_success or count completions_success/error: the
    // caller already observed one outcome (kFailed) for this logical
    // request and must not see a second, contradictory one.
    ++diag(in_flight_owner_).late_completions;
    releaseSlot(in_flight_owner_);
    return;
  }

  if (mine.event_ready) {
    ++diag(in_flight_owner_).spurious_events;
    return;
  }

  mine.event_ready = true;
  mine.event_success = (evt_id == NRF_EVT_FLASH_OPERATION_SUCCESS);
}

void FlashMutationGate::pumpEvents() {
  if (!softDeviceEnabled()) return;

  if (bluefruitOwnsSocEvents()) {
    // Bluefruit is now the sole sd_evt_get() consumer. Its pinned SoC-task
    // patch forwards only the gate-owned flash completion into this one-word
    // mailbox. InternalFS completions are filtered by the shared owner token.
    const uint32_t bridged = takeBridgedGateEvent();
    if (bridged == 1U) handleFlashEvent(NRF_EVT_FLASH_OPERATION_SUCCESS);
    else if (bridged == 2U) handleFlashEvent(NRF_EVT_FLASH_OPERATION_ERROR);
    return;
  }

  // Pre-BLE behavior: this gate remains the sole SoC queue drainer.
  uint32_t evt_id = 0;
  while (sd_evt_get(&evt_id) == NRF_SUCCESS) {
    if (evt_id != NRF_EVT_FLASH_OPERATION_SUCCESS &&
        evt_id != NRF_EVT_FLASH_OPERATION_ERROR)
      continue;
    handleFlashEvent(evt_id);
  }
}

}  // namespace orun_tlp
