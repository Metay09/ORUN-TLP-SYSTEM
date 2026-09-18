// M7P3: FlashMutationGate async contract, against the actual production
// class. Only the Nordic SVC boundary (sd_flash_write/sd_flash_page_erase/
// sd_softdevice_is_enabled/sd_evt_get) is faked, fully controllable per call.
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

TestFicr ficr{kPageSize, 256};
TestUicr uicr{{0xF4000}};
TestFicr* NRF_FICR = &ficr;
TestUicr* NRF_UICR = &uicr;

namespace {
uint32_t fake_now_ms = 0;
}
// Test-controlled clock instead of linking the real RTOS-backed
// monotonic_time.cpp, so the bounded timeout can be driven deterministically.
namespace orun_tlp::monotonic {
uint32_t nowMs() { return fake_now_ms; }
}  // namespace orun_tlp::monotonic

namespace {
bool sd_enabled = false;
std::vector<uint32_t> submit_results;  // queued results for the next sd_flash_* call(s).
std::vector<uint32_t> event_queue;     // queued event ids for sd_evt_get, in order.
unsigned write_calls = 0, erase_calls = 0, evt_calls = 0;

uint32_t nextSubmitResult() {
  if (submit_results.empty()) return NRF_SUCCESS;
  const uint32_t r = submit_results.front();
  submit_results.erase(submit_results.begin());
  return r;
}
void reset() {
  sd_enabled = false;
  submit_results.clear();
  event_queue.clear();
  write_calls = erase_calls = evt_calls = 0;
}
}  // namespace

uint32_t sd_softdevice_is_enabled(uint8_t* out) { *out = sd_enabled ? 1 : 0; return NRF_SUCCESS; }

uint32_t sd_flash_write(uint32_t* dst, const uint32_t* src, uint32_t words) {
  ++write_calls;
  const uint32_t result = nextSubmitResult();
  if (result != NRF_SUCCESS) return result;
  // Real hardware begins the physical write as soon as the command is
  // accepted; only software completion confirmation is deferred to the
  // later event. Modelled the same way here.
  for (uint32_t i = 0; i < words; ++i) dst[i] &= src[i];
  return NRF_SUCCESS;
}

uint32_t sd_flash_page_erase(uint32_t page) {
  ++erase_calls;
  const uint32_t result = nextSubmitResult();
  if (result != NRF_SUCCESS) return result;
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

int main() {
  void* region = mmap(reinterpret_cast<void*>(kBaseAddress), kRegionSize,
                      PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
  assert(region == reinterpret_cast<void*>(kBaseAddress));
  memset(region, 0xFF, kRegionSize);

  // Each block below constructs its own FlashMutationGate: diagnostics()
  // counters and in-flight state are per-instance, so a fresh gate per
  // block keeps each check's expected counts self-contained and exact,
  // rather than cumulative across the whole file.

  // J: SoftDevice-disabled legacy path is synchronous and behaviorally
  // identical to the pre-M7P3 NrfHistoryFlash contract -- never kPending,
  // never touches the async plumbing (write_calls counts the same
  // underlying SVC either way, but no event round trip is needed).
  {
    reset();
    FlashMutationGate gate;
    assert(gate.begin());
    alignas(4) uint8_t data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    assert(gate.program(0, data, 8) == FlashOpResult::kDone);
    assert(memcmp(region, data, 8) == 0);
    assert(write_calls == 1 && evt_calls == 0);
    assert(gate.diagnostics().async_accepted == 0);
    assert(gate.erasePage(0) == FlashOpResult::kDone);
    for (uint32_t i = 0; i < kPageSize; ++i)
      assert(static_cast<uint8_t*>(region)[i] == 0xFF);
  }

  // A: simple async write -- accepted, pending before the event, completes
  // on the matching success event, caller notified exactly once.
  {
    reset();
    FlashMutationGate gate;
    assert(gate.begin());
    sd_enabled = true;
    memset(region, 0xFF, kRegionSize);
    alignas(4) uint8_t data[8] = {10, 20, 30, 40, 50, 60, 70, 80};
    assert(gate.program(64, data, 8) == FlashOpResult::kPending);
    assert(write_calls == 1);
    assert(memcmp(static_cast<uint8_t*>(region) + 64, data, 8) == 0);  // hardware already wrote
    assert(gate.pollPending() == FlashOpResult::kPending);  // no event yet
    event_queue.push_back(NRF_EVT_FLASH_OPERATION_SUCCESS);
    gate.pumpEvents();
    assert(gate.pollPending() == FlashOpResult::kDone);  // exactly-once notification:
    assert(gate.pollPending() == FlashOpResult::kFailed);  // nothing in flight any more
    assert(gate.diagnostics().completions_success == 1);
  }

  // B: async error -- operation fails, gate returns to a reusable state,
  // no false success is ever reported for this request.
  {
    reset();
    FlashMutationGate gate;
    assert(gate.begin());
    sd_enabled = true;
    alignas(4) uint8_t data[4] = {9, 9, 9, 9};
    assert(gate.program(128, data, 4) == FlashOpResult::kPending);
    event_queue.push_back(NRF_EVT_FLASH_OPERATION_ERROR);
    gate.pumpEvents();
    assert(gate.pollPending() == FlashOpResult::kFailed);
    assert(gate.diagnostics().completions_error == 1);
    // Reusable: a fresh submission works normally afterward.
    assert(gate.program(132, data, 4) == FlashOpResult::kPending);
    event_queue.push_back(NRF_EVT_FLASH_OPERATION_SUCCESS);
    gate.pumpEvents();
    assert(gate.pollPending() == FlashOpResult::kDone);
  }

  // C: stable source-buffer ownership -- the caller's buffer is mutated
  // immediately after program() returns kPending; the eventually-submitted
  // bytes must reflect the ORIGINAL data, proving the gate staged its own
  // copy rather than keeping a pointer into caller-owned memory.
  {
    reset();
    FlashMutationGate gate;
    assert(gate.begin());
    sd_enabled = true;
    memset(region, 0xFF, kRegionSize);
    alignas(4) uint8_t data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    const uint8_t original[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    assert(gate.program(256, data, 8) == FlashOpResult::kPending);
    memset(data, 0xAA, sizeof(data));  // caller buffer corrupted post-submit
    event_queue.push_back(NRF_EVT_FLASH_OPERATION_SUCCESS);
    gate.pumpEvents();
    assert(gate.pollPending() == FlashOpResult::kDone);
    assert(memcmp(static_cast<uint8_t*>(region) + 256, original, 8) == 0);
  }

  // D: NRF_ERROR_BUSY on submission must not spin or block; it is retried
  // from pollPending() (normal service/poll context), bounded, and a later
  // submission succeeds without the caller resubmitting data.
  {
    reset();
    FlashMutationGate gate;
    assert(gate.begin());
    sd_enabled = true;
    memset(region, 0xFF, kRegionSize);
    submit_results = {NRF_ERROR_BUSY, NRF_ERROR_BUSY, NRF_SUCCESS};
    alignas(4) uint8_t data[4] = {7, 7, 7, 7};
    assert(gate.program(384, data, 4) == FlashOpResult::kPending);
    assert(write_calls == 1 && gate.diagnostics().busy_retries == 1);
    assert(gate.pollPending() == FlashOpResult::kPending);  // still busy
    assert(write_calls == 2 && gate.diagnostics().busy_retries == 2);
    assert(gate.pollPending() == FlashOpResult::kPending);  // accepted now, awaiting event
    assert(write_calls == 3 && gate.diagnostics().async_accepted == 1);
    event_queue.push_back(NRF_EVT_FLASH_OPERATION_SUCCESS);
    gate.pumpEvents();
    assert(gate.pollPending() == FlashOpResult::kDone);
    assert(memcmp(static_cast<uint8_t*>(region) + 384, data, 4) == 0);
  }

  // E: no overlapping operations -- a second mutation attempted while one
  // is in flight is rejected outright, and does not disturb the first.
  {
    reset();
    FlashMutationGate gate;
    assert(gate.begin());
    sd_enabled = true;
    memset(region, 0xFF, kRegionSize);
    alignas(4) uint8_t first[4] = {1, 1, 1, 1};
    alignas(4) uint8_t second[4] = {2, 2, 2, 2};
    assert(gate.program(512, first, 4) == FlashOpResult::kPending);
    assert(gate.program(516, second, 4) == FlashOpResult::kFailed);  // rejected, not queued
    assert(write_calls == 1);  // the rejected call never reached the SVC
    event_queue.push_back(NRF_EVT_FLASH_OPERATION_SUCCESS);
    gate.pumpEvents();
    assert(gate.pollPending() == FlashOpResult::kDone);  // first request unaffected
    assert(memcmp(static_cast<uint8_t*>(region) + 512, first, 4) == 0);
  }

  // I: duplicate/spurious flash events must not complete the wrong request
  // or double-call completion.
  {
    reset();
    FlashMutationGate gate;
    assert(gate.begin());
    sd_enabled = true;
    // Spurious event with nothing in flight at all: drained, not fatal.
    event_queue.push_back(NRF_EVT_FLASH_OPERATION_SUCCESS);
    gate.pumpEvents();
    assert(gate.diagnostics().spurious_events == 1);

    memset(region, 0xFF, kRegionSize);
    alignas(4) uint8_t data[4] = {3, 3, 3, 3};
    assert(gate.program(640, data, 4) == FlashOpResult::kPending);
    // Two success events queued before the caller ever polls: the second
    // must be treated as spurious, not a second completion.
    event_queue.push_back(NRF_EVT_FLASH_OPERATION_SUCCESS);
    event_queue.push_back(NRF_EVT_FLASH_OPERATION_SUCCESS);
    gate.pumpEvents();
    assert(gate.diagnostics().spurious_events == 2);
    assert(gate.pollPending() == FlashOpResult::kDone);
    assert(gate.diagnostics().completions_success == 1);
  }

  // Timeout: a lost/missing completion event must not wedge the device
  // forever. After the bounded timeout, the request fails closed and the
  // gate becomes reusable again for a fresh request.
  {
    reset();
    FlashMutationGate gate;
    assert(gate.begin());
    sd_enabled = true;
    alignas(4) uint8_t data[4] = {5, 5, 5, 5};
    assert(gate.program(768, data, 4) == FlashOpResult::kPending);
    assert(gate.pollPending() == FlashOpResult::kPending);  // event never arrives
    fake_now_ms += 10000;  // well past the bounded timeout
    assert(gate.pollPending() == FlashOpResult::kFailed);
    assert(gate.diagnostics().timeouts == 1);
    // Reusable afterward, and a stray late event for the abandoned request
    // does not retroactively complete anything.
    event_queue.push_back(NRF_EVT_FLASH_OPERATION_SUCCESS);
    gate.pumpEvents();
    assert(gate.diagnostics().spurious_events == 1);
    assert(gate.program(772, data, 4) == FlashOpResult::kPending);
    event_queue.push_back(NRF_EVT_FLASH_OPERATION_SUCCESS);
    gate.pumpEvents();
    assert(gate.pollPending() == FlashOpResult::kDone);
  }

  assert(munmap(region, kRegionSize) == 0);
  puts("M7P3 FlashMutationGate async contract checks: PASS");
}
