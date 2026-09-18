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
  ++write_calls;
  for (uint32_t i = 0; i < words; ++i) dst[i] &= src[i];
  return NRF_SUCCESS;
}

uint32_t sd_flash_page_erase(uint32_t page) {
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
  sd_enabled = true;
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

  assert(munmap(history_region, kRegionSize) == 0);
  puts("M7P7A BLE flash/event arbitration checks: PASS");
}
