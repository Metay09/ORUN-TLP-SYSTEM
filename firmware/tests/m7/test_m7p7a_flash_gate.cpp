// M7P7A: focused ownership tests for the bridge between FlashMutationGate
// and the patched Bluefruit/InternalFS framework. BLE itself is not enabled.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <vector>

#include <Arduino.h>
#include <nrf_sdm.h>

#include "flash_mutation_gate.h"

using namespace orun_tlp;
using namespace orun_tlp::storage_config;

extern "C" bool orun_flash_internalfs_try_acquire(void);
extern "C" void orun_flash_internalfs_release(void);
extern "C" bool orun_flash_internalfs_owns(void);
extern "C" void orun_flash_gate_soc_event_cb(uint32_t event);
extern "C" void orun_flash_gate_set_bluefruit_soc_owner(bool active);

TestFicr ficr{kPageSize, 256};
TestUicr uicr{{0xF4000}};
TestFicr* NRF_FICR = &ficr;
TestUicr* NRF_UICR = &uicr;

namespace {
uint32_t fake_now_ms = 0;
bool sd_enabled = false;
// When true, sd_flash_write/sd_flash_page_erase report NRF_ERROR_BUSY and
// perform no mutation -- simulates SoftDevice never accepting a submission,
// so callers can exercise the pre-acceptance timeout/retry path.
bool force_busy = false;
std::vector<uint32_t> event_queue;
unsigned write_calls = 0;
unsigned erase_calls = 0;
unsigned evt_calls = 0;
}

namespace orun_tlp::monotonic {
uint32_t nowMs() { return fake_now_ms; }
}

uint32_t sd_softdevice_is_enabled(uint8_t* out) {
  *out = sd_enabled ? 1 : 0;
  return NRF_SUCCESS;
}

uint32_t sd_flash_write(uint32_t* dst, const uint32_t* src, uint32_t words) {
  if (force_busy) return NRF_ERROR_BUSY;
  ++write_calls;
  for (uint32_t i = 0; i < words; ++i) dst[i] &= src[i];
  return NRF_SUCCESS;
}

uint32_t sd_flash_page_erase(uint32_t page) {
  if (force_busy) return NRF_ERROR_BUSY;
  ++erase_calls;
  memset(reinterpret_cast<void*>(uintptr_t(page) * kPageSize), 0xFF, kPageSize);
  return NRF_SUCCESS;
}

uint32_t sd_evt_get(uint32_t* out) {
  ++evt_calls;
  if (event_queue.empty()) return NRF_ERROR_NOT_FOUND;
  *out = event_queue.front();
  event_queue.erase(event_queue.begin());
  return NRF_SUCCESS;
}

static void resetHarness() {
  fake_now_ms = 0;
  // Production boot recovers stores before Bluefruit enables SoftDevice.
  // NrfHistoryFlash::begin() intentionally fails closed if SoftDevice is
  // already active, so each focused scenario enables the fake SoftDevice
  // only after gate.begin() succeeds.
  sd_enabled = false;
  force_busy = false;
  event_queue.clear();
  write_calls = erase_calls = evt_calls = 0;
  orun_flash_gate_set_bluefruit_soc_owner(false);
  if (orun_flash_internalfs_owns()) orun_flash_internalfs_release();
}

int main() {
  void* history_region = mmap(reinterpret_cast<void*>(kBaseAddress), kRegionSize,
                              PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                              -1, 0);
  assert(history_region == reinterpret_cast<void*>(kBaseAddress));
  memset(history_region, 0xFF, kRegionSize);

  // 1. InternalFS physical ownership excludes an ORUN submission. The
  // request remains pending and submits immediately after InternalFS releases.
  {
    resetHarness();
    FlashMutationGate gate;
    assert(gate.begin());
    sd_enabled = true;
    assert(orun_flash_internalfs_try_acquire());
    assert(orun_flash_internalfs_owns());

    alignas(4) uint8_t data[4] = {1, 2, 3, 4};
    assert(gate.program(0, data, sizeof(data)) == FlashOpResult::kPending);
    assert(write_calls == 0);

    orun_flash_internalfs_release();
    assert(!orun_flash_internalfs_owns());
    assert(gate.pollPending() == FlashOpResult::kPending);
    assert(write_calls == 1);

    event_queue.push_back(NRF_EVT_FLASH_OPERATION_SUCCESS);
    gate.pumpEvents();
    assert(gate.pollPending() == FlashOpResult::kDone);
    assert(memcmp(history_region, data, sizeof(data)) == 0);
  }

  // 2. Once Bluefruit owns sd_evt_get(), FlashMutationGate must stop draining
  // that queue. Completion comes only through the forwarded bridge callback.
  {
    resetHarness();
    memset(history_region, 0xFF, kRegionSize);
    FlashMutationGate gate;
    assert(gate.begin());
    sd_enabled = true;
    orun_flash_gate_set_bluefruit_soc_owner(true);

    alignas(4) uint8_t data[4] = {5, 6, 7, 8};
    assert(gate.program(4, data, sizeof(data)) == FlashOpResult::kPending);
    assert(write_calls == 1);

    // If pumpEvents() still called sd_evt_get(), this queued event would be
    // consumed and evt_calls would increment. Neither is allowed now.
    event_queue.push_back(NRF_EVT_FLASH_OPERATION_SUCCESS);
    gate.pumpEvents();
    assert(evt_calls == 0);
    assert(event_queue.size() == 1);
    assert(gate.pollPending() == FlashOpResult::kPending);

    // Simulate Bluefruit's SoC task forwarding the real completion.
    orun_flash_gate_soc_event_cb(NRF_EVT_FLASH_OPERATION_SUCCESS);
    gate.pumpEvents();
    assert(gate.pollPending() == FlashOpResult::kDone);
    assert(memcmp(static_cast<uint8_t*>(history_region) + 4,
                  data, sizeof(data)) == 0);
  }

  // 3. A flash event observed while InternalFS owns the physical token must
  // not become a stale ORUN-gate completion.
  {
    resetHarness();
    memset(history_region, 0xFF, kRegionSize);
    FlashMutationGate gate;
    assert(gate.begin());
    sd_enabled = true;
    orun_flash_gate_set_bluefruit_soc_owner(true);

    assert(orun_flash_internalfs_try_acquire());
    orun_flash_gate_soc_event_cb(NRF_EVT_FLASH_OPERATION_SUCCESS);
    orun_flash_internalfs_release();

    alignas(4) uint8_t data[4] = {9, 10, 11, 12};
    assert(gate.program(8, data, sizeof(data)) == FlashOpResult::kPending);
    gate.pumpEvents();
    assert(gate.pollPending() == FlashOpResult::kPending);

    orun_flash_gate_soc_event_cb(NRF_EVT_FLASH_OPERATION_SUCCESS);
    gate.pumpEvents();
    assert(gate.pollPending() == FlashOpResult::kDone);
    assert(memcmp(static_cast<uint8_t*>(history_region) + 8,
                  data, sizeof(data)) == 0);
  }

  // 4. A queued ORUN request must not falsely time out merely because
  // InternalFS is legitimately still within ITS OWN bounded wait for the
  // shared token (patch_ble_flash.py's ORUN_FLASH_ARBITER_WAIT_MS, 4500ms),
  // even though that already exceeds the single kOperationTimeoutMs
  // (4000ms) budget every later phase used to share with this one. It must
  // still eventually time out (safely -- nothing was ever submitted) if
  // InternalFS never releases the token at all.
  {
    resetHarness();
    memset(history_region, 0xFF, kRegionSize);
    FlashMutationGate gate;
    assert(gate.begin());
    sd_enabled = true;
    assert(orun_flash_internalfs_try_acquire());  // InternalFS holds the token.

    alignas(4) uint8_t data[4] = {1, 2, 3, 4};
    assert(gate.program(0, data, sizeof(data)) == FlashOpResult::kPending);
    assert(write_calls == 0);

    // Past the old, shared 4000ms budget -- must still be waiting, not failed.
    fake_now_ms = 4200;
    assert(gate.pollPending() == FlashOpResult::kPending);
    assert(write_calls == 0);
    assert(gate.diagnostics().timeouts == 0);

    orun_flash_internalfs_release();
    assert(gate.pollPending() == FlashOpResult::kPending);
    assert(write_calls == 1);

    event_queue.push_back(NRF_EVT_FLASH_OPERATION_SUCCESS);
    gate.pumpEvents();
    assert(gate.pollPending() == FlashOpResult::kDone);
  }
  {
    resetHarness();
    memset(history_region, 0xFF, kRegionSize);
    FlashMutationGate gate;
    assert(gate.begin());
    sd_enabled = true;
    assert(orun_flash_internalfs_try_acquire());  // InternalFS holds the token forever.

    alignas(4) uint8_t data[4] = {1, 2, 3, 4};
    assert(gate.program(0, data, sizeof(data)) == FlashOpResult::kPending);

    fake_now_ms = 6000;  // Now past the token-wait budget itself.
    assert(gate.pollPending() == FlashOpResult::kFailed);
    assert(gate.diagnostics().timeouts == 1);
    assert(write_calls == 0);
    // Nothing was ever submitted: the token must have been released cleanly
    // (no quarantine), so InternalFS (still the current holder in this
    // scenario) is unaffected and ORUN itself can acquire fresh next time.
    orun_flash_internalfs_release();
    assert(orun_flash_internalfs_try_acquire());
    orun_flash_internalfs_release();
  }

  // 5. Once SoftDevice has accepted a physical operation (submission_accepted),
  // a logical/application timeout must quarantine the shared token, not
  // release it: InternalFS must not be able to acquire ownership while this
  // operation's physical completion is still unresolved.
  FlashMutationGate quarantine_gate;
  {
    resetHarness();
    memset(history_region, 0xFF, kRegionSize);
    assert(quarantine_gate.begin());
    sd_enabled = true;

    alignas(4) uint8_t data[4] = {21, 22, 23, 24};
    assert(quarantine_gate.program(0, data, sizeof(data)) == FlashOpResult::kPending);
    assert(write_calls == 1);  // SoftDevice accepted the write; no completion event yet.

    fake_now_ms = 4001;  // Past kOperationTimeoutMs since token acquisition, no event.
    assert(quarantine_gate.pollPending() == FlashOpResult::kFailed);
    assert(quarantine_gate.diagnostics().timeouts == 1);

    // The physical token must still be held by the gate -- InternalFS cannot
    // acquire it while this operation's outcome is unknown.
    assert(!orun_flash_internalfs_try_acquire());
    assert(!orun_flash_internalfs_owns());

    // A stray resubmission attempt from the same owner must also be
    // rejected (kind != kNone) until the quarantine resolves, not silently
    // start a second physical operation.
    alignas(4) uint8_t retry[4] = {0, 0, 0, 0};
    assert(quarantine_gate.program(0, retry, sizeof(retry)) == FlashOpResult::kFailed);
    assert(write_calls == 1);  // No second sd_flash_write was attempted.
  }

  // 6. The late completion, once it finally arrives, must be reconciled as
  // ORUN's own event -- never misattributed to InternalFS merely because it
  // happens to arrive after InternalFS could otherwise have raced in.
  {
    event_queue.push_back(NRF_EVT_FLASH_OPERATION_SUCCESS);
    quarantine_gate.pumpEvents();
    assert(quarantine_gate.diagnostics().late_completions == 1);
    // Not double-counted: the caller already observed kFailed from the
    // timeout above, so this must not also report a success completion.
    assert(quarantine_gate.diagnostics().completions_success == 0);
    assert(quarantine_gate.diagnostics().completions_error == 0);
    assert(!orun_flash_internalfs_owns());
  }

  // 7. After that late completion is reconciled, ownership can safely become
  // available again -- for both InternalFS and a fresh ORUN request.
  {
    assert(orun_flash_internalfs_try_acquire());
    orun_flash_internalfs_release();

    alignas(4) uint8_t more[4] = {31, 32, 33, 34};
    assert(quarantine_gate.program(4, more, sizeof(more)) == FlashOpResult::kPending);
    assert(write_calls == 2);
    event_queue.push_back(NRF_EVT_FLASH_OPERATION_SUCCESS);
    quarantine_gate.pumpEvents();
    assert(quarantine_gate.pollPending() == FlashOpResult::kDone);
    assert(memcmp(static_cast<uint8_t*>(history_region) + 4,
                  more, sizeof(more)) == 0);
  }

  // 8. A submission that never gets past NRF_ERROR_BUSY (SoftDevice never
  // accepts it) must still time out and cleanly release the shared token --
  // this is entirely pre-acceptance, so quarantine must NOT apply.
  {
    resetHarness();
    memset(history_region, 0xFF, kRegionSize);
    FlashMutationGate gate;
    assert(gate.begin());
    sd_enabled = true;
    force_busy = true;

    alignas(4) uint8_t data[4] = {41, 42, 43, 44};
    assert(gate.program(0, data, sizeof(data)) == FlashOpResult::kPending);
    assert(write_calls == 0);  // Never actually accepted.

    fake_now_ms = 4001;
    assert(gate.pollPending() == FlashOpResult::kFailed);
    assert(gate.diagnostics().timeouts == 1);

    // Never submitted to SoftDevice: safe to hand the token straight back,
    // no quarantine.
    assert(orun_flash_internalfs_try_acquire());
    orun_flash_internalfs_release();
    force_busy = false;
  }

  assert(munmap(history_region, kRegionSize) == 0);
  puts("M7P7A BLE flash/event arbitration checks: PASS");
}
