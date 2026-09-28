#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#include <vector>

#include <Arduino.h>
#include <nrf_sdm.h>

#include "flash_mutation_gate.h"
#include "storage_config.h"

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
}

namespace {
bool sd_enabled = false;
std::vector<uint32_t> submit_results;
std::vector<uint32_t> events;
unsigned write_calls = 0;
unsigned erase_calls = 0;

uint32_t nextSubmit() {
  if (submit_results.empty()) return NRF_SUCCESS;
  const uint32_t result = submit_results.front();
  submit_results.erase(submit_results.begin());
  return result;
}

void resetHarness() {
  fake_now_ms = 0;
  sd_enabled = false;
  submit_results.clear();
  events.clear();
  write_calls = 0;
  erase_calls = 0;
}
}

uint32_t sd_softdevice_is_enabled(uint8_t* out) {
  *out = sd_enabled ? 1 : 0;
  return NRF_SUCCESS;
}

uint32_t sd_flash_write(uint32_t* dst, const uint32_t* src, uint32_t words) {
  ++write_calls;
  const uint32_t result = nextSubmit();
  if (result != NRF_SUCCESS) return result;
  for (uint32_t i = 0; i < words; ++i) dst[i] &= src[i];
  return NRF_SUCCESS;
}

uint32_t sd_flash_page_erase(uint32_t page) {
  ++erase_calls;
  const uint32_t result = nextSubmit();
  if (result != NRF_SUCCESS) return result;
  memset(reinterpret_cast<void*>(uintptr_t(page) * kPageSize), 0xFF,
         kPageSize);
  return NRF_SUCCESS;
}

uint32_t sd_evt_get(uint32_t* out) {
  if (events.empty()) return NRF_ERROR_NOT_FOUND;
  *out = events.front();
  events.erase(events.begin());
  return NRF_SUCCESS;
}

int main() {
  constexpr uint32_t kConfigSize =
      kFutureConfigRegionEnd - kFutureConfigRegionStart;
  constexpr uint32_t kSecuritySize =
      kFutureSecurityRegionEnd - kFutureSecurityRegionStart;
  constexpr uint32_t kGeofenceSize =
      kGeofenceRegionEnd - kGeofenceRegionStart;

  void* history = mmap(reinterpret_cast<void*>(kBaseAddress), kRegionSize,
                       PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                       -1, 0);
  void* config = mmap(reinterpret_cast<void*>(kFutureConfigRegionStart),
                      kConfigSize, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                      -1, 0);
  void* security = mmap(reinterpret_cast<void*>(kFutureSecurityRegionStart),
                        kSecuritySize, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                        -1, 0);
  void* geofence = mmap(reinterpret_cast<void*>(kGeofenceRegionStart),
                        kGeofenceSize, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                        -1, 0);
  assert(history == reinterpret_cast<void*>(kBaseAddress));
  assert(config == reinterpret_cast<void*>(kFutureConfigRegionStart));
  assert(security == reinterpret_cast<void*>(kFutureSecurityRegionStart));
  assert(geofence == reinterpret_cast<void*>(kGeofenceRegionStart));

  memset(history, 0xFF, kRegionSize);
  memset(config, 0xFF, kConfigSize);
  memset(security, 0xFF, kSecuritySize);
  memset(geofence, 0xFF, kGeofenceSize);

  // 1. SoftDevice-disabled path owns exactly the geofence region and accepts
  // the real M6D3A body+CRC size (560 bytes).
  {
    resetHarness();
    FlashMutationGate gate;
    assert(gate.geofencePort().begin());

    alignas(4) uint8_t body[560];
    for (unsigned i = 0; i < sizeof(body); ++i)
      body[i] = static_cast<uint8_t>(i + 1);

    assert(gate.geofencePort().program(0, body, sizeof(body)) ==
           FlashOpResult::kDone);
    assert(memcmp(geofence, body, sizeof(body)) == 0);
    assert(gate.geofencePort().program(kGeofenceSize - 4, body, 8) ==
           FlashOpResult::kFailed);
    assert(gate.geofencePort().erasePage(kGeofenceRegionPages) ==
           FlashOpResult::kFailed);
    assert(gate.geofencePort().erasePage(0) == FlashOpResult::kDone);
    for (uint32_t i = 0; i < kPageSize; ++i)
      assert(static_cast<uint8_t*>(geofence)[i] == 0xFF);
  }

  // 2. Config outranks Geofence. Hold SEC_MAINT admitted/BUSY so both can
  // stage; once it completes, polling Geofence first must still defer to
  // already-staged Config.
  {
    resetHarness();
    FlashMutationGate gate;
    assert(gate.configPort().begin());
    assert(gate.geofencePort().begin());
    assert(gate.securityMaintPort().begin());
    sd_enabled = true;
    memset(config, 0xFF, kConfigSize);
    memset(geofence, 0xFF, kGeofenceSize);
    memset(security, 0xFF, kSecuritySize);

    submit_results = {NRF_ERROR_BUSY};
    assert(gate.securityMaintPort().erasePage(0) == FlashOpResult::kPending);

    alignas(4) uint8_t c[4] = {1, 2, 3, 4};
    alignas(4) uint8_t g[4] = {5, 6, 7, 8};
    assert(gate.configPort().program(0, c, sizeof(c)) ==
           FlashOpResult::kPending);
    assert(gate.geofencePort().program(0, g, sizeof(g)) ==
           FlashOpResult::kPending);

    submit_results.clear();
    assert(gate.securityMaintPort().pollPending() == FlashOpResult::kPending);
    events.push_back(NRF_EVT_FLASH_OPERATION_SUCCESS);
    gate.pumpEvents();
    assert(gate.securityMaintPort().pollPending() == FlashOpResult::kDone);

    const unsigned before = write_calls;
    assert(gate.geofencePort().pollPending() == FlashOpResult::kPending);
    assert(write_calls == before);  // Config is waiting with higher priority.

    assert(gate.configPort().pollPending() == FlashOpResult::kPending);
    assert(write_calls == before + 1);
    events.push_back(NRF_EVT_FLASH_OPERATION_SUCCESS);
    gate.pumpEvents();
    assert(gate.configPort().pollPending() == FlashOpResult::kDone);

    assert(gate.geofencePort().pollPending() == FlashOpResult::kPending);
    events.push_back(NRF_EVT_FLASH_OPERATION_SUCCESS);
    gate.pumpEvents();
    assert(gate.geofencePort().pollPending() == FlashOpResult::kDone);
  }

  // 3. Geofence outranks SEC_MAINT. Hold History admitted/BUSY while both
  // stage, then poll SEC_MAINT first after release; it must defer.
  {
    resetHarness();
    FlashMutationGate gate;
    assert(gate.begin());
    assert(gate.geofencePort().begin());
    assert(gate.securityMaintPort().begin());
    sd_enabled = true;
    memset(history, 0xFF, kRegionSize);
    memset(geofence, 0xFF, kGeofenceSize);
    memset(security, 0xFF, kSecuritySize);

    submit_results = {NRF_ERROR_BUSY};
    alignas(4) uint8_t h[4] = {9, 9, 9, 9};
    assert(gate.program(0, h, sizeof(h)) == FlashOpResult::kPending);

    alignas(4) uint8_t g[4] = {7, 7, 7, 7};
    assert(gate.geofencePort().program(0, g, sizeof(g)) ==
           FlashOpResult::kPending);
    assert(gate.securityMaintPort().erasePage(0) ==
           FlashOpResult::kPending);

    submit_results.clear();
    assert(gate.pollPending() == FlashOpResult::kPending);
    events.push_back(NRF_EVT_FLASH_OPERATION_SUCCESS);
    gate.pumpEvents();
    assert(gate.pollPending() == FlashOpResult::kDone);

    const unsigned erase_before = erase_calls;
    assert(gate.securityMaintPort().pollPending() == FlashOpResult::kPending);
    assert(erase_calls == erase_before);

    assert(gate.geofencePort().pollPending() == FlashOpResult::kPending);
    events.push_back(NRF_EVT_FLASH_OPERATION_SUCCESS);
    gate.pumpEvents();
    assert(gate.geofencePort().pollPending() == FlashOpResult::kDone);

    assert(gate.securityMaintPort().pollPending() == FlashOpResult::kPending);
    assert(erase_calls == erase_before + 1);
    events.push_back(NRF_EVT_FLASH_OPERATION_SUCCESS);
    gate.pumpEvents();
    assert(gate.securityMaintPort().pollPending() == FlashOpResult::kDone);
  }

  // 4. The full 560-byte source is copied into gate-owned staging before
  // asynchronous submission; caller mutation after return cannot change it.
  {
    resetHarness();
    FlashMutationGate gate;
    assert(gate.geofencePort().begin());
    sd_enabled = true;
    memset(geofence, 0xFF, kGeofenceSize);

    alignas(4) uint8_t body[560];
    uint8_t expected[560];
    for (unsigned i = 0; i < sizeof(body); ++i)
      body[i] = expected[i] = static_cast<uint8_t>(i ^ 0xA5U);

    assert(gate.geofencePort().program(0, body, sizeof(body)) ==
           FlashOpResult::kPending);
    memset(body, 0, sizeof(body));

    events.push_back(NRF_EVT_FLASH_OPERATION_SUCCESS);
    gate.pumpEvents();
    assert(gate.geofencePort().pollPending() == FlashOpResult::kDone);
    assert(memcmp(geofence, expected, sizeof(expected)) == 0);
  }

  // 5. Anti-starvation: an aged Geofence request promotes to top effective
  // priority. A fresh Config request staged the instant History releases the
  // slot must wait for the already-aged geofence mutation.
  {
    resetHarness();
    FlashMutationGate gate;
    assert(gate.begin());
    assert(gate.configPort().begin());
    assert(gate.geofencePort().begin());
    sd_enabled = true;
    memset(history, 0xFF, kRegionSize);
    memset(config, 0xFF, kConfigSize);
    memset(geofence, 0xFF, kGeofenceSize);

    submit_results = {NRF_ERROR_BUSY};
    alignas(4) uint8_t h[4] = {3, 3, 3, 3};
    assert(gate.program(0, h, sizeof(h)) == FlashOpResult::kPending);

    alignas(4) uint8_t g[4] = {4, 4, 4, 4};
    assert(gate.geofencePort().program(0, g, sizeof(g)) ==
           FlashOpResult::kPending);

    fake_now_ms = 4001;
    submit_results.clear();
    assert(gate.pollPending() == FlashOpResult::kPending);
    events.push_back(NRF_EVT_FLASH_OPERATION_SUCCESS);
    gate.pumpEvents();
    assert(gate.pollPending() == FlashOpResult::kDone);

    alignas(4) uint8_t c[4] = {5, 5, 5, 5};
    const unsigned before = write_calls;
    assert(gate.configPort().program(0, c, sizeof(c)) ==
           FlashOpResult::kPending);
    assert(write_calls == before);  // aged Geofence outranks fresh Config.

    assert(gate.geofencePort().pollPending() == FlashOpResult::kPending);
    assert(write_calls == before + 1);
    events.push_back(NRF_EVT_FLASH_OPERATION_SUCCESS);
    gate.pumpEvents();
    assert(gate.geofencePort().pollPending() == FlashOpResult::kDone);

    assert(gate.configPort().pollPending() == FlashOpResult::kPending);
    events.push_back(NRF_EVT_FLASH_OPERATION_SUCCESS);
    gate.pumpEvents();
    assert(gate.configPort().pollPending() == FlashOpResult::kDone);
  }

  // 6. Accepted operation timeout quarantines Geofence exactly like the older
  // clients: application sees failure, physical token remains owned until the
  // definitive late completion event reconciles it.
  {
    resetHarness();
    FlashMutationGate gate;
    assert(gate.geofencePort().begin());
    sd_enabled = true;
    memset(geofence, 0xFF, kGeofenceSize);

    alignas(4) uint8_t data[4] = {1, 1, 1, 1};
    assert(gate.geofencePort().program(0, data, sizeof(data)) ==
           FlashOpResult::kPending);
    fake_now_ms = 4001;
    assert(gate.geofencePort().pollPending() == FlashOpResult::kFailed);
    assert(gate.geofenceMutationUnreconciled());
    assert(gate.geofenceDiagnostics().timeouts == 1);

    events.push_back(NRF_EVT_FLASH_OPERATION_SUCCESS);
    gate.pumpEvents();
    assert(!gate.geofenceMutationUnreconciled());
    assert(gate.geofenceDiagnostics().late_completions == 1);
  }

  assert(munmap(history, kRegionSize) == 0);
  assert(munmap(config, kConfigSize) == 0);
  assert(munmap(security, kSecuritySize) == 0);
  assert(munmap(geofence, kGeofenceSize) == 0);

  puts("M6D3B FlashMutationGate geofence client checks: PASS");
}
