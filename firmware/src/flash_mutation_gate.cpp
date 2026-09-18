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
constexpr uint32_t kOperationTimeoutMs = 4000;

bool inBounds(uint32_t offset, size_t size) {
  return offset <= kRegionSize && size <= kRegionSize - offset;
}
}  // namespace

bool FlashMutationGate::begin() {
  ready_ = sync_backend_.begin();
  return ready_;
}

bool FlashMutationGate::read(uint32_t offset, void* data, size_t size) const {
  return sync_backend_.read(offset, data, size);
}

bool FlashMutationGate::softDeviceEnabled() const {
  uint8_t enabled = 1;
  return sd_softdevice_is_enabled(&enabled) == NRF_SUCCESS && enabled != 0;
}

bool FlashMutationGate::timedOut(uint32_t now) const {
  return monotonic::elapsed(now, in_flight_started_ms_, kOperationTimeoutMs);
}

void FlashMutationGate::resetInFlight() {
  in_flight_kind_ = Kind::kNone;
  submission_accepted_ = false;
  event_ready_ = false;
  staging_size_ = 0;
}

FlashOpResult FlashMutationGate::program(uint32_t offset, const void* data, size_t size) {
  if (!ready_) return FlashOpResult::kFailed;
  ++diagnostics_.submits;
  // Deliberately re-checks SoftDevice state here even though
  // sync_backend_.program() performs its own independent
  // synchronousFlashAvailable() check when this delegates to it: two
  // independent guards checking the same fact is intentional defense in
  // depth, not redundancy to remove -- NrfHistoryFlash's own guard must stay
  // intact unweakened (see its class comment), and this gate's guard is what
  // decides which of the two entirely different code paths (sync delegate
  // vs. raw async SVCs) to take in the first place.
  if (!softDeviceEnabled()) return sync_backend_.program(offset, data, size);

  // Async path: validate exactly as the synchronous backend does (same
  // storage_config bounds, alignment, and erased-destination precondition),
  // then stage an owned copy before issuing the request -- the source
  // buffer's lifetime is not guaranteed by the caller past this call.
  if (in_flight_kind_ != Kind::kNone) return FlashOpResult::kFailed;
  if (!data || !size || !inBounds(offset, size) || (offset & 3U) != 0 ||
      (size & 3U) != 0 || size > sizeof(staging_) ||
      size > kPageSize - offset % kPageSize)
    return FlashOpResult::kFailed;
  const auto* destination = reinterpret_cast<const uint8_t*>(kBaseAddress + offset);
  for (size_t index = 0; index < size; ++index)
    if (destination[index] != 0xFF) return FlashOpResult::kFailed;

  memcpy(staging_, data, size);
  staging_size_ = size;
  in_flight_kind_ = Kind::kProgram;
  in_flight_target_ = kBaseAddress + offset;
  in_flight_started_ms_ = monotonic::nowMs();
  submission_accepted_ = false;
  event_ready_ = false;
  return attemptSubmit();
}

FlashOpResult FlashMutationGate::erasePage(uint32_t page) {
  if (!ready_) return FlashOpResult::kFailed;
  ++diagnostics_.submits;
  if (!softDeviceEnabled()) return sync_backend_.erasePage(page);

  if (in_flight_kind_ != Kind::kNone) return FlashOpResult::kFailed;
  if (page >= kPageCount) return FlashOpResult::kFailed;

  in_flight_kind_ = Kind::kErase;
  in_flight_target_ = kBaseAddress / kPageSize + page;
  in_flight_started_ms_ = monotonic::nowMs();
  submission_accepted_ = false;
  event_ready_ = false;
  return attemptSubmit();
}

FlashOpResult FlashMutationGate::attemptSubmit() {
  uint32_t result = NRF_ERROR_INTERNAL;
  if (in_flight_kind_ == Kind::kProgram) {
    result = sd_flash_write(reinterpret_cast<uint32_t*>(in_flight_target_),
                            reinterpret_cast<const uint32_t*>(staging_),
                            staging_size_ / sizeof(uint32_t));
  } else if (in_flight_kind_ == Kind::kErase) {
    result = sd_flash_page_erase(in_flight_target_);
  }

  if (result == NRF_SUCCESS) {
    submission_accepted_ = true;
    ++diagnostics_.async_accepted;
    return FlashOpResult::kPending;
  }
  if (result == NRF_ERROR_BUSY) {
    ++diagnostics_.busy_retries;
    if (timedOut(monotonic::nowMs())) {
      resetInFlight();
      ++diagnostics_.timeouts;
      return FlashOpResult::kFailed;
    }
    return FlashOpResult::kPending;  // Retry submission from pollPending().
  }
  // Permanent rejection (invalid address/length, forbidden region, or an
  // internal SoftDevice error opening the session): fail closed now rather
  // than retrying a request the SoftDevice has already refused to start.
  resetInFlight();
  return FlashOpResult::kFailed;
}

FlashOpResult FlashMutationGate::pollPending() {
  if (in_flight_kind_ == Kind::kNone) return FlashOpResult::kFailed;
  if (!submission_accepted_) return attemptSubmit();
  if (event_ready_) {
    const bool ok = event_success_;
    resetInFlight();
    if (ok) {
      ++diagnostics_.completions_success;
      return FlashOpResult::kDone;
    }
    ++diagnostics_.completions_error;
    return FlashOpResult::kFailed;
  }
  if (timedOut(monotonic::nowMs())) {
    // A lost/missing completion event must not wedge the device: fail this
    // request closed at the application level. This is not a claim that the
    // physical write/erase did or did not happen -- the flash backend never
    // asserts durability from a timeout, only from a confirmed SUCCESS
    // event or, when SoftDevice is disabled, an immediate verified readback.
    resetInFlight();
    ++diagnostics_.timeouts;
    return FlashOpResult::kFailed;
  }
  return FlashOpResult::kPending;
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
    if (in_flight_kind_ == Kind::kNone || !submission_accepted_ || event_ready_) {
      // No matching in-flight request, or a second flash event arrived
      // before pollPending() consumed the first (the SoftDevice API
      // documents exactly one event per command, so this should not
      // happen) -- discard rather than completing the wrong request or
      // double-calling completion.
      ++diagnostics_.spurious_events;
      continue;
    }
    event_ready_ = true;
    event_success_ = (evt_id == NRF_EVT_FLASH_OPERATION_SUCCESS);
  }
}

}  // namespace orun_tlp
