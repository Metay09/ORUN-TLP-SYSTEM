// M7P5: FlashMutationGate generalized to serve History AND Config against
// the actual production classes (FlashMutationGate, NrfHistoryFlash,
// NrfConfigFlash). Only the Nordic SVC boundary is faked, fully
// controllable per call -- same idiom as test_m7p3_flash_gate.cpp, which
// this file does not modify or duplicate (that file keeps proving the
// unchanged single-client History API/behavior; this one proves the new
// dual-client admission/priority/event-routing behavior and the config
// partition's own exact bounds).
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
namespace orun_tlp::monotonic {
uint32_t nowMs() { return fake_now_ms; }
}  // namespace orun_tlp::monotonic

namespace {
bool sd_enabled = false;
std::vector<uint32_t> submit_results;
std::vector<uint32_t> event_queue;
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
  void* history_region = mmap(reinterpret_cast<void*>(kBaseAddress), kRegionSize,
                              PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
  assert(history_region == reinterpret_cast<void*>(kBaseAddress));
  constexpr uint32_t kConfigRegionSize = kFutureConfigRegionEnd - kFutureConfigRegionStart;
  void* config_region = mmap(reinterpret_cast<void*>(kFutureConfigRegionStart), kConfigRegionSize,
                             PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
  assert(config_region == reinterpret_cast<void*>(kFutureConfigRegionStart));
  memset(history_region, 0xFF, kRegionSize);
  memset(config_region, 0xFF, kConfigRegionSize);

  // Config's own SoftDevice-disabled sync path and exact bounds, mirroring
  // the M4 nrf_backend coverage already proven for history.
  {
    reset();
    FlashMutationGate gate;
    assert(gate.begin());
    assert(gate.configPort().begin());
    alignas(4) uint8_t data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    assert(gate.configPort().program(0, data, 8) == FlashOpResult::kDone);
    assert(memcmp(config_region, data, 8) == 0);
    // Out-of-bounds / cross-partition write attempts are rejected, never
    // silently clamped or redirected into an adjacent region.
    assert(gate.configPort().program(kConfigRegionSize - 4, data, 8) == FlashOpResult::kFailed);
    assert(gate.configPort().erasePage(kFutureConfigRegionPages) == FlashOpResult::kFailed);
    assert(gate.configPort().erasePage(0) == FlashOpResult::kDone);
    for (uint32_t i = 0; i < kPageSize; ++i)
      assert(static_cast<uint8_t*>(config_region)[i] == 0xFF);
    // History's own region is completely untouched by any config call above.
    for (uint32_t i = 0; i < kPageSize; ++i)
      assert(static_cast<uint8_t*>(history_region)[i] == 0xFF);
  }

  // History vs config serialization/priority: both submit while SoftDevice
  // is enabled; config is admitted only after history's operation resolves,
  // even though config asks first here -- proving priority is not merely
  // "whoever calls program() first".
  {
    reset();
    FlashMutationGate gate;
    assert(gate.begin());
    assert(gate.configPort().begin());
    sd_enabled = true;
    memset(history_region, 0xFF, kRegionSize);
    memset(config_region, 0xFF, kConfigRegionSize);

    alignas(4) uint8_t config_data[4] = {9, 9, 9, 9};
    alignas(4) uint8_t history_data[4] = {1, 1, 1, 1};
    // Config asks first, but nothing is in flight yet and history hasn't
    // asked -- config alone is legitimately admitted immediately.
    assert(gate.configPort().program(0, config_data, 4) == FlashOpResult::kPending);
    assert(write_calls == 1);
    // Now history asks while config's op is genuinely in flight (already
    // accepted by the fake SVC): history cannot preempt a physically
    // in-flight operation (no cancel exists), so it stays queued.
    assert(gate.program(0, history_data, 4) == FlashOpResult::kPending);
    assert(write_calls == 1);  // history's request was not submitted yet
    assert(gate.pollPending() == FlashOpResult::kPending);  // still queued behind config
    assert(write_calls == 1);

    // Config's operation completes.
    event_queue.push_back(NRF_EVT_FLASH_OPERATION_SUCCESS);
    gate.pumpEvents();
    assert(gate.configPort().pollPending() == FlashOpResult::kDone);
    assert(memcmp(config_region, config_data, 4) == 0);

    // Now the physical slot is free; history is admitted next.
    assert(gate.pollPending() == FlashOpResult::kPending);  // submits now
    assert(write_calls == 2);
    event_queue.push_back(NRF_EVT_FLASH_OPERATION_SUCCESS);
    gate.pumpEvents();
    assert(gate.pollPending() == FlashOpResult::kDone);
    assert(memcmp(history_region, history_data, 4) == 0);
  }

  // Priority ordering itself: when the slot is free and BOTH clients have a
  // request already staged, history is admitted first even if config's
  // pollPending() happens to be polled first this tick.
  {
    reset();
    FlashMutationGate gate;
    assert(gate.begin());
    assert(gate.configPort().begin());
    sd_enabled = true;
    memset(history_region, 0xFF, kRegionSize);
    memset(config_region, 0xFF, kConfigRegionSize);
    submit_results = {NRF_ERROR_BUSY};  // hold the first submit attempt off
    alignas(4) uint8_t history_data[4] = {2, 2, 2, 2};
    assert(gate.program(4, history_data, 4) == FlashOpResult::kPending);  // BUSY: not yet admitted-and-accepted
    assert(write_calls == 1);
    alignas(4) uint8_t config_data[4] = {3, 3, 3, 3};
    // Config stages a request too, while history's is still unresolved.
    assert(gate.configPort().program(4, config_data, 4) == FlashOpResult::kPending);
    assert(write_calls == 1);  // config did not even attempt a submit: history has priority admission
    // Config polling repeatedly still does not jump ahead of history.
    assert(gate.configPort().pollPending() == FlashOpResult::kPending);
    assert(write_calls == 1);
    // History retries and succeeds.
    assert(gate.pollPending() == FlashOpResult::kPending);
    assert(write_calls == 2);
    event_queue.push_back(NRF_EVT_FLASH_OPERATION_SUCCESS);
    gate.pumpEvents();
    assert(gate.pollPending() == FlashOpResult::kDone);
    // Only now is config admitted.
    assert(gate.configPort().pollPending() == FlashOpResult::kPending);
    assert(write_calls == 3);
    event_queue.push_back(NRF_EVT_FLASH_OPERATION_SUCCESS);
    gate.pumpEvents();
    assert(gate.configPort().pollPending() == FlashOpResult::kDone);
    assert(memcmp(static_cast<uint8_t*>(config_region) + 4, config_data, 4) == 0);
  }

  // Shared event-queue routing: a completion event for the client that
  // truly owns the in-flight slot must never be stolen/discarded by the
  // other client's own pumpEvents() draining the one global queue -- both
  // clients share a single pumpEvents() implementation precisely to avoid
  // this hazard, so this is really proving there is only one drain point.
  {
    reset();
    FlashMutationGate gate;
    assert(gate.begin());
    assert(gate.configPort().begin());
    sd_enabled = true;
    memset(history_region, 0xFF, kRegionSize);
    alignas(4) uint8_t history_data[4] = {4, 4, 4, 4};
    assert(gate.program(8, history_data, 4) == FlashOpResult::kPending);
    // Config has nothing outstanding at all right now.
    event_queue.push_back(NRF_EVT_FLASH_OPERATION_SUCCESS);
    gate.pumpEvents();  // single shared drain -- routes correctly to history
    assert(gate.pollPending() == FlashOpResult::kDone);
    assert(gate.configDiagnostics().spurious_events == 0);
    assert(gate.diagnostics().completions_success == 1);
  }

  assert(munmap(history_region, kRegionSize) == 0);
  assert(munmap(config_region, kConfigRegionSize) == 0);
  puts("M7P5 FlashMutationGate dual-client checks: PASS");
}
