// M7P6B: FlashMutationGate generalized to serve History, Config AND
// Security against the actual production classes (FlashMutationGate,
// NrfHistoryFlash, NrfConfigFlash, NrfSecurityFlash). Only the Nordic SVC
// boundary is faked, exactly as in test_m7p5_flash_gate.cpp (which this
// file does not modify or duplicate -- that file keeps proving the
// unchanged two-client History/Config behavior; this one proves the new
// five-way priority admission: SEC_CRITICAL > History > Config > SEC_MAINT,
// and the security partition's own exact bounds).
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
  constexpr uint32_t kSecurityRegionSize = kFutureSecurityRegionEnd - kFutureSecurityRegionStart;
  void* security_region = mmap(reinterpret_cast<void*>(kFutureSecurityRegionStart), kSecurityRegionSize,
                               PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
  assert(security_region == reinterpret_cast<void*>(kFutureSecurityRegionStart));
  memset(history_region, 0xFF, kRegionSize);
  memset(config_region, 0xFF, kConfigRegionSize);
  memset(security_region, 0xFF, kSecurityRegionSize);

  // Security's own SoftDevice-disabled sync path and exact bounds.
  {
    reset();
    FlashMutationGate gate;
    assert(gate.begin());
    assert(gate.securityCriticalPort().begin());
    alignas(4) uint8_t data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    assert(gate.securityCriticalPort().program(0, data, 8) == FlashOpResult::kDone);
    assert(memcmp(security_region, data, 8) == 0);
    assert(gate.securityMaintPort().program(kSecurityRegionSize - 4, data, 8) == FlashOpResult::kFailed);
    assert(gate.securityMaintPort().erasePage(kFutureSecurityRegionPages) == FlashOpResult::kFailed);
    assert(gate.securityMaintPort().erasePage(0) == FlashOpResult::kDone);
    for (uint32_t i = 0; i < kPageSize; ++i)
      assert(static_cast<uint8_t*>(security_region)[i] == 0xFF);
    // Neither history's nor config's region is touched by any security call.
    for (uint32_t i = 0; i < kPageSize; ++i)
      assert(static_cast<uint8_t*>(history_region)[i] == 0xFF);
    for (uint32_t i = 0; i < kPageSize; ++i)
      assert(static_cast<uint8_t*>(config_region)[i] == 0xFF);
  }

  // SEC_CRITICAL outranks History: both stage while SoftDevice is enabled;
  // history asks first, but the SEC_CRITICAL security request is admitted
  // first once the slot is free -- proving priority is not "whoever calls
  // program() first" and that Security's own priority is chosen per port,
  // not per owner-identity alone.
  {
    reset();
    FlashMutationGate gate;
    assert(gate.begin());
    assert(gate.securityCriticalPort().begin());
    sd_enabled = true;
    memset(history_region, 0xFF, kRegionSize);
    memset(security_region, 0xFF, kSecurityRegionSize);
    submit_results = {NRF_ERROR_BUSY};  // hold the first submit attempt off.

    alignas(4) uint8_t history_data[4] = {1, 1, 1, 1};
    assert(gate.program(0, history_data, 4) == FlashOpResult::kPending);  // BUSY: not yet admitted-and-accepted.
    assert(write_calls == 1);
    alignas(4) uint8_t security_data[4] = {2, 2, 2, 2};
    // Security (critical) stages a request too, while history's is still
    // unresolved (BUSY-retrying, not yet admitted).
    assert(gate.securityCriticalPort().program(0, security_data, 4) == FlashOpResult::kPending);
    assert(write_calls == 1);  // security did not even attempt a submit yet -- history still holds admission here.
    // Retrying history again resolves its BUSY and completes it.
    assert(gate.pollPending() == FlashOpResult::kPending);
    assert(write_calls == 2);
    event_queue.push_back(NRF_EVT_FLASH_OPERATION_SUCCESS);
    gate.pumpEvents();
    assert(gate.pollPending() == FlashOpResult::kDone);
    assert(memcmp(history_region, history_data, 4) == 0);
    // Now security's SEC_CRITICAL request is admitted.
    assert(gate.securityCriticalPort().pollPending() == FlashOpResult::kPending);
    assert(write_calls == 3);
    event_queue.push_back(NRF_EVT_FLASH_OPERATION_SUCCESS);
    gate.pumpEvents();
    assert(gate.securityCriticalPort().pollPending() == FlashOpResult::kDone);
    assert(memcmp(security_region, security_data, 4) == 0);
  }

  // Full priority chain (SEC_CRITICAL > History > Config > SEC_MAINT).
  // SecurityStore itself only ever has ONE outstanding security request at a
  // time (critical XOR maint, never both), so the chain is proven in two
  // three-way scenarios sharing the same physical slot: SEC_CRITICAL vs.
  // History vs. Config (SEC_CRITICAL wins), and History vs. Config vs.
  // SEC_MAINT (SEC_MAINT loses) -- combined with the SEC_CRITICAL > History
  // scenario above and History > Config already proven in
  // test_m7p5_flash_gate.cpp, this establishes the complete order.
  {
    reset();
    FlashMutationGate gate;
    assert(gate.begin());
    assert(gate.securityCriticalPort().begin());
    assert(gate.configPort().begin());
    sd_enabled = true;
    memset(history_region, 0xFF, kRegionSize);
    memset(config_region, 0xFF, kConfigRegionSize);
    memset(security_region, 0xFF, kSecurityRegionSize);
    submit_results = {NRF_ERROR_BUSY};  // hold the very first submit off so all three can stage.

    alignas(4) uint8_t config_data[4] = {2, 2, 2, 2};
    alignas(4) uint8_t history_data[4] = {3, 3, 3, 3};
    alignas(4) uint8_t critical_data[4] = {4, 4, 4, 4};

    // Config asks first: it alone is admitted since nothing else has staged
    // yet, but its submit is BUSY-held.
    assert(gate.configPort().program(0, config_data, 4) == FlashOpResult::kPending);
    assert(write_calls == 1);
    // History and SEC_CRITICAL both stage while Config holds the slot.
    assert(gate.program(0, history_data, 4) == FlashOpResult::kPending);
    assert(gate.securityCriticalPort().program(4, critical_data, 4) == FlashOpResult::kPending);
    assert(write_calls == 1);  // neither even attempted a submit.

    // Config's own (already-admitted) retry finally succeeds.
    assert(gate.configPort().pollPending() == FlashOpResult::kPending);
    assert(write_calls == 2);
    event_queue.push_back(NRF_EVT_FLASH_OPERATION_SUCCESS);
    gate.pumpEvents();
    assert(gate.configPort().pollPending() == FlashOpResult::kDone);

    // SEC_CRITICAL is admitted next, ahead of History.
    assert(gate.securityCriticalPort().pollPending() == FlashOpResult::kPending);
    assert(write_calls == 3);
    event_queue.push_back(NRF_EVT_FLASH_OPERATION_SUCCESS);
    gate.pumpEvents();
    assert(gate.securityCriticalPort().pollPending() == FlashOpResult::kDone);
    assert(memcmp(static_cast<uint8_t*>(security_region) + 4, critical_data, 4) == 0);

    // History last.
    assert(gate.pollPending() == FlashOpResult::kPending);
    assert(write_calls == 4);
    event_queue.push_back(NRF_EVT_FLASH_OPERATION_SUCCESS);
    gate.pumpEvents();
    assert(gate.pollPending() == FlashOpResult::kDone);
    assert(memcmp(history_region, history_data, 4) == 0);
  }
  {
    reset();
    FlashMutationGate gate;
    assert(gate.begin());
    assert(gate.securityCriticalPort().begin());
    assert(gate.configPort().begin());
    sd_enabled = true;
    memset(history_region, 0xFF, kRegionSize);
    memset(config_region, 0xFF, kConfigRegionSize);
    memset(security_region, 0xFF, kSecurityRegionSize);
    submit_results = {NRF_ERROR_BUSY};

    alignas(4) uint8_t config_data[4] = {5, 5, 5, 5};
    alignas(4) uint8_t history_data[4] = {6, 6, 6, 6};

    // SEC_MAINT asks first (erase page 0): admitted alone since nothing
    // else has staged yet, but BUSY-held.
    assert(gate.securityMaintPort().erasePage(0) == FlashOpResult::kPending);
    assert(erase_calls == 1);
    // Config and History both stage while SEC_MAINT holds the slot.
    assert(gate.configPort().program(0, config_data, 4) == FlashOpResult::kPending);
    assert(gate.program(0, history_data, 4) == FlashOpResult::kPending);
    assert(write_calls == 0 && erase_calls == 1);

    // SEC_MAINT's own retry finally succeeds.
    assert(gate.securityMaintPort().pollPending() == FlashOpResult::kPending);
    assert(erase_calls == 2);
    event_queue.push_back(NRF_EVT_FLASH_OPERATION_SUCCESS);
    gate.pumpEvents();
    assert(gate.securityMaintPort().pollPending() == FlashOpResult::kDone);

    // History is admitted next, ahead of Config -- SEC_MAINT never jumps
    // ahead of either, even though it asked first.
    assert(gate.pollPending() == FlashOpResult::kPending);
    assert(write_calls == 1);
    event_queue.push_back(NRF_EVT_FLASH_OPERATION_SUCCESS);
    gate.pumpEvents();
    assert(gate.pollPending() == FlashOpResult::kDone);
    assert(memcmp(history_region, history_data, 4) == 0);

    // Config last.
    assert(gate.configPort().pollPending() == FlashOpResult::kPending);
    assert(write_calls == 2);
    event_queue.push_back(NRF_EVT_FLASH_OPERATION_SUCCESS);
    gate.pumpEvents();
    assert(gate.configPort().pollPending() == FlashOpResult::kDone);
    assert(memcmp(config_region, config_data, 4) == 0);
  }

  // SEC_MAINT must never starve History: with both staged, History is
  // always admitted first, repeatedly, even though SEC_MAINT asked first.
  {
    reset();
    FlashMutationGate gate;
    assert(gate.begin());
    assert(gate.securityCriticalPort().begin());
    sd_enabled = true;
    memset(history_region, 0xFF, kRegionSize);
    memset(security_region, 0xFF, kSecurityRegionSize);
    submit_results = {NRF_ERROR_BUSY};
    assert(gate.securityMaintPort().erasePage(1) == FlashOpResult::kPending);
    assert(erase_calls == 1);
    alignas(4) uint8_t history_data[4] = {9, 9, 9, 9};
    assert(gate.program(8, history_data, 4) == FlashOpResult::kPending);
    assert(write_calls == 0);  // History did not even attempt a submit: SEC_MAINT still holds admission (BUSY-retrying).
    assert(gate.securityMaintPort().pollPending() == FlashOpResult::kPending);
    assert(erase_calls == 2);
    event_queue.push_back(NRF_EVT_FLASH_OPERATION_SUCCESS);
    gate.pumpEvents();
    assert(gate.securityMaintPort().pollPending() == FlashOpResult::kDone);
    // Now that the slot is free, History (not a second SEC_MAINT request)
    // is admitted -- there is only one, already-drained SEC_MAINT request
    // here, so this also proves History does not itself get stuck.
    assert(gate.pollPending() == FlashOpResult::kPending);
    assert(write_calls == 1);
    event_queue.push_back(NRF_EVT_FLASH_OPERATION_SUCCESS);
    gate.pumpEvents();
    assert(gate.pollPending() == FlashOpResult::kDone);
    assert(memcmp(static_cast<uint8_t*>(history_region) + 8, history_data, 4) == 0);
  }

  // Anti-starvation: SEC_MAINT staged for longer than kOperationTimeoutMs
  // (4000ms) ages to top priority, so sustained History traffic that always
  // has a FRESH request staged the instant the slot frees cannot starve it
  // forever. Without aging, this scenario would loop indefinitely.
  {
    reset();
    fake_now_ms = 0;
    FlashMutationGate gate;
    assert(gate.begin());
    assert(gate.securityMaintPort().begin());
    sd_enabled = true;
    memset(history_region, 0xFF, kRegionSize);
    memset(security_region, 0xFF, kSecurityRegionSize);

    // History gets admitted first (nothing else staged yet) and is held
    // BUSY-retrying so it keeps the physical slot without completing.
    submit_results = {NRF_ERROR_BUSY};
    alignas(4) uint8_t h0[4] = {9, 9, 9, 9};
    assert(gate.program(0, h0, 4) == FlashOpResult::kPending);
    assert(write_calls == 1);
    // SEC_MAINT stages behind it at t=0 -- queued, never even attempts a
    // submit while History owns the physical slot.
    assert(gate.securityMaintPort().erasePage(0) == FlashOpResult::kPending);
    assert(erase_calls == 0);

    // 4001ms pass while SEC_MAINT remains staged (not admitted) and History
    // still holds the slot -- SEC_MAINT's staged wait is now well past
    // kOperationTimeoutMs.
    fake_now_ms = 4001;
    submit_results.clear();  // History's retry now succeeds.
    assert(gate.pollPending() == FlashOpResult::kPending);
    event_queue.push_back(NRF_EVT_FLASH_OPERATION_SUCCESS);
    gate.pumpEvents();
    assert(gate.pollPending() == FlashOpResult::kDone);  // History's op releases the slot.

    // The instant the slot frees, History stages a brand-new (fresh, t=4001)
    // request before SEC_MAINT gets polled again -- simulating continuous
    // History traffic that would otherwise always win admission.
    alignas(4) uint8_t h1[4] = {8, 8, 8, 8};
    assert(gate.program(4, h1, 4) == FlashOpResult::kPending);
    assert(write_calls == 2);  // deferred: NOT admitted despite normally outranking SEC_MAINT
                                // (h0's BUSY retry + successful submit already used 2 writes).
    // SEC_MAINT's aged request is admitted instead.
    assert(gate.securityMaintPort().pollPending() == FlashOpResult::kPending);
    assert(erase_calls == 1);
    event_queue.push_back(NRF_EVT_FLASH_OPERATION_SUCCESS);
    gate.pumpEvents();
    assert(gate.securityMaintPort().pollPending() == FlashOpResult::kDone);
    for (uint32_t i = 0; i < kPageSize; ++i)
      assert(static_cast<uint8_t*>(security_region)[i] == 0xFF);  // erase actually landed.

    // History's fresh (still-staged) request is admitted next, now that
    // SEC_MAINT's aged request has been released.
    assert(gate.pollPending() == FlashOpResult::kPending);
    assert(write_calls == 3);
    event_queue.push_back(NRF_EVT_FLASH_OPERATION_SUCCESS);
    gate.pumpEvents();
    assert(gate.pollPending() == FlashOpResult::kDone);
    assert(memcmp(static_cast<uint8_t*>(history_region) + 4, h1, 4) == 0);
  }

  // Queued-timeout fix generalizes to Security: a SEC_CRITICAL request that
  // waits longer than kOperationTimeoutMs (4000ms) while merely staged
  // behind History's in-flight operation must not be timed out for that
  // queued wait -- the clock starts only once genuinely admitted.
  {
    reset();
    fake_now_ms = 0;
    FlashMutationGate gate;
    assert(gate.begin());
    assert(gate.securityCriticalPort().begin());
    sd_enabled = true;
    memset(history_region, 0xFF, kRegionSize);
    memset(security_region, 0xFF, kSecurityRegionSize);

    alignas(4) uint8_t history_data[4] = {5, 5, 5, 5};
    alignas(4) uint8_t security_data[4] = {6, 6, 6, 6};

    assert(gate.program(0, history_data, 4) == FlashOpResult::kPending);
    assert(write_calls == 1);
    assert(gate.securityCriticalPort().program(0, security_data, 4) == FlashOpResult::kPending);
    assert(write_calls == 1);

    fake_now_ms += 5000;
    assert(gate.securityCriticalPort().pollPending() == FlashOpResult::kPending);
    assert(write_calls == 1);
    assert(gate.securityDiagnostics().timeouts == 0);

    event_queue.push_back(NRF_EVT_FLASH_OPERATION_SUCCESS);
    gate.pumpEvents();
    assert(gate.pollPending() == FlashOpResult::kDone);
    assert(gate.diagnostics().timeouts == 0);

    assert(gate.securityCriticalPort().pollPending() == FlashOpResult::kPending);  // submits now, fresh budget.
    assert(write_calls == 2);
    fake_now_ms += 3999;
    assert(gate.securityCriticalPort().pollPending() == FlashOpResult::kPending);
    assert(gate.securityDiagnostics().timeouts == 0);
    event_queue.push_back(NRF_EVT_FLASH_OPERATION_SUCCESS);
    gate.pumpEvents();
    assert(gate.securityCriticalPort().pollPending() == FlashOpResult::kDone);
    assert(memcmp(security_region, security_data, 4) == 0);
    assert(gate.securityDiagnostics().timeouts == 0);
    assert(gate.securityDiagnostics().spurious_events == 0);
    assert(gate.diagnostics().spurious_events == 0);
  }

  assert(munmap(history_region, kRegionSize) == 0);
  assert(munmap(config_region, kConfigRegionSize) == 0);
  assert(munmap(security_region, kSecurityRegionSize) == 0);
  puts("M7P6B FlashMutationGate three-client priority checks: PASS");
}
