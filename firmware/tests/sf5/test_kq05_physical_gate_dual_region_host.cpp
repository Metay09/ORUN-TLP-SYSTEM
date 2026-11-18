// KQ05 PC-only physical address and shared SoftDevice FLASH arbitration
// contract. This program mmap's synthetic NOR at the real nRF52 address
// ranges and links the ACTUAL FlashMutationGate + NrfProduct/HistoryFlash.
// NO physical RAK USB, firmware upload, real Flash erase or migration.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <vector>

#include <Arduino.h>
#include <nrf_sdm.h>

#include "flash_mutation_gate.h"
#include "kq05_segmented_flash_host.h"

using namespace orun_tlp;
using namespace orun_tlp::storage_config;
namespace k = orun_tlp::kq05_segmented_host;
namespace s = orun_tlp::kq05_supplemental;

TestFicr ficr{kPageSize, 256};
TestUicr uicr{{0xF4000}};
TestFicr* NRF_FICR = &ficr;
TestUicr* NRF_UICR = &uicr;

namespace {
bool sd_enabled = false;
uint32_t fake_now_ms = 0U;
std::vector<uint32_t> events;
uint32_t write_calls = 0U;
uint32_t erase_calls = 0U;
uint8_t* primary = nullptr;
uint8_t* supplemental = nullptr;
uint8_t* geofence = nullptr;
uint8_t* security = nullptr;
uint8_t* config = nullptr;
uint8_t* bond = nullptr;

constexpr uint64_t kTrackerId = UINT64_C(0x0102030405060708);

uint8_t* mapAt(uint32_t base, uint32_t size) {
  void* m = mmap(reinterpret_cast<void*>(uintptr_t(base)), size,
      PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS |
      MAP_FIXED_NOREPLACE, -1, 0);
  assert(m == reinterpret_cast<void*>(uintptr_t(base)));
  memset(m, 0xFF, size);
  return static_cast<uint8_t*>(m);
}

void clearRegions() {
  memset(primary, 0xFF, k::kPrimaryBytes);
  memset(supplemental, 0xFF, k::kSupplementalBytes);
  memset(geofence, 0xFF, kGeofenceRegionPages * kPageSize);
  memset(security, 0xFF, kFutureSecurityRegionPages * kPageSize);
  memset(config, 0xFF, kFutureConfigRegionPages * kPageSize);
  memset(bond, 0xFF, kFutureBondRegionPages * kPageSize);
  fake_now_ms = 0U;
  write_calls = erase_calls = 0U;
  events.clear();
  sd_enabled = false;
}

bool allErased(const uint8_t* ptr, size_t size) {
  for (size_t i = 0U; i < size; ++i)
    if (ptr[i] != 0xFFU) return false;
  return true;
}

void assertOtherPartitionsUnchanged() {
  assert(allErased(geofence, kGeofenceRegionPages * kPageSize));
  assert(allErased(security, kFutureSecurityRegionPages * kPageSize));
  assert(allErased(config, kFutureConfigRegionPages * kPageSize));
  assert(allErased(bond, kFutureBondRegionPages * kPageSize));
}
} // namespace

namespace orun_tlp {
namespace monotonic {
uint32_t nowMs() { return fake_now_ms; }
}
}

uint32_t sd_softdevice_is_enabled(uint8_t* out) {
  *out = sd_enabled ? 1U : 0U;
  return NRF_SUCCESS;
}

uint32_t sd_flash_write(uint32_t* dst, const uint32_t* src, uint32_t words) {
  ++write_calls;
  for (uint32_t i = 0U; i < words; ++i) dst[i] &= src[i];
  return NRF_SUCCESS;
}

uint32_t sd_flash_page_erase(uint32_t page) {
  ++erase_calls;
  memset(reinterpret_cast<void*>(uintptr_t(page) * kPageSize),
         0xFF, kPageSize);
  return NRF_SUCCESS;
}

uint32_t sd_evt_get(uint32_t* result) {
  if (events.empty()) return NRF_ERROR_NOT_FOUND;
  *result = events.front();
  events.erase(events.begin());
  return NRF_SUCCESS;
}

namespace {

void testExclusiveOwnerAndPhysicalBoundaries() {
  clearRegions();
  FlashMutationGate gate;
  // 'History' must not concurrently own the old region at the same time
  // as the segmented owner. These two guards are simulated in HOST only.
  k::SegmentedFlash denied(gate.productPort(), gate,
      s::Role::kTracker, kTrackerId, false, true);
  assert(!denied.begin());

  k::SegmentedFlash segmented(gate.productPort(), gate,
      s::Role::kTracker, kTrackerId, true, true);
  assert(segmented.begin());
  const uint8_t product_value[4] = {0x11U, 0x22U, 0x33U, 0x44U};
  const uint8_t history_value[4] = {0x55U, 0x66U, 0x77U, 0x88U};
  assert(segmented.program(0U, product_value, sizeof(product_value)) ==
         FlashOpResult::kDone);
  assert(segmented.program(k::kPrimaryBytes, history_value,
                           sizeof(history_value)) == FlashOpResult::kDone);
  assert(memcmp(primary, product_value, sizeof(product_value)) == 0);
  assert(memcmp(supplemental, history_value, sizeof(history_value)) == 0);
  assert(segmented.program(k::kPrimaryBytes - 2U, product_value, 4U) ==
         FlashOpResult::kFailed);
  assert(segmented.erasePage(k::kTotalPages) == FlashOpResult::kFailed);

  // No operation has interpreted the 32 KiB of Geofence/Security/Config/Bond
  // real physical data as part of the logical 156 KiB region.
  assertOtherPartitionsUnchanged();
  assert(erase_calls == 0U);
}

void testSharedSoftdeviceMutationSlot() {
  clearRegions();
  FlashMutationGate gate;
  k::SegmentedFlash segmented(gate.productPort(), gate,
      s::Role::kTracker, kTrackerId, true, true);
  assert(segmented.begin());  // bootstrap before BLE/SoftDevice turns on
  sd_enabled = true;

  const uint8_t history_bytes[4] = {0x10U, 0x20U, 0x30U, 0x40U};
  const uint8_t product_bytes[4] = {0x50U, 0x60U, 0x70U, 0x80U};

  assert(segmented.program(k::kPrimaryBytes, history_bytes, 4U) ==
         FlashOpResult::kPending);
  assert(write_calls == 1U);
  assert(segmented.pending());

  // A second independent gate client cannot submit another physical SVC
  // until history's exact completion event has been consumed.
  const FlashOpResult staged = gate.productPort().program(
      0U, product_bytes, 4U);
  assert(staged == FlashOpResult::kPending);
  assert(write_calls == 1U);

  events.push_back(NRF_EVT_FLASH_OPERATION_SUCCESS);
  gate.pumpEvents();
  assert(segmented.pollPending() == FlashOpResult::kDone);
  assert(memcmp(supplemental, history_bytes, 4U) == 0);

  // The queued Product owner receives the shared token only after History.
  assert(gate.productPort().pollPending() == FlashOpResult::kPending);
  assert(write_calls == 2U);
  events.push_back(NRF_EVT_FLASH_OPERATION_SUCCESS);
  gate.pumpEvents();
  assert(gate.productPort().pollPending() == FlashOpResult::kDone);
  assert(memcmp(primary, product_bytes, 4U) == 0);
  assert(erase_calls == 0U);
  assertOtherPartitionsUnchanged();
}

void testHistoryTimeoutQuarantineKeepsFlashOwned() {
  clearRegions();
  FlashMutationGate gate;
  k::SegmentedFlash segmented(gate.productPort(), gate,
      s::Role::kTracker, kTrackerId, true, true);
  assert(segmented.begin());
  assert(!gate.hasUnreconciledMutation());

  sd_enabled = true;
  const uint8_t history_bytes[4] = {0xA1U, 0xB2U, 0xC3U, 0xD4U};
  const uint8_t product_bytes[4] = {0x01U, 0x02U, 0x03U, 0x04U};
  assert(segmented.program(k::kPrimaryBytes, history_bytes, 4U) ==
         FlashOpResult::kPending);
  assert(write_calls == 1U);
  assert(!gate.hasUnreconciledMutation());

  // Nordic accepted the operation but its completion event is missing.
  // A 4s application deadline must NOT free the physical ownership token.
  fake_now_ms = 4001U;
  assert(segmented.pollPending() == FlashOpResult::kFailed);
  assert(gate.diagnostics().timeouts == 1U);
  assert(gate.hasUnreconciledMutation());
  assert(!segmented.ready());
  assert(!segmented.begin());
  assert(segmented.program(k::kPrimaryBytes + 4U,
             product_bytes, 4U) == FlashOpResult::kFailed);

  // Even an independent Product client must wait for the actual History
  // SVC completion event. It may stage, but cannot submit a second SVC.
  assert(gate.productPort().program(0U, product_bytes, 4U) ==
         FlashOpResult::kPending);
  assert(write_calls == 1U);
  assert(gate.productPort().pollPending() == FlashOpResult::kPending);
  assert(write_calls == 1U);

  // The late event reconciles physical ownership, NOT application success:
  // the old segmented store stays permanently faulted and cannot release
  // any observation/custody record based solely on this event.
  events.push_back(NRF_EVT_FLASH_OPERATION_SUCCESS);
  gate.pumpEvents();
  assert(!gate.hasUnreconciledMutation());
  assert(gate.diagnostics().late_completions == 1U);
  assert(gate.diagnostics().completions_success == 0U);
  assert(!segmented.ready());

  assert(gate.productPort().pollPending() == FlashOpResult::kPending);
  assert(write_calls == 2U);
  events.push_back(NRF_EVT_FLASH_OPERATION_SUCCESS);
  gate.pumpEvents();
  assert(gate.productPort().pollPending() == FlashOpResult::kDone);
  assertOtherPartitionsUnchanged();
  assert(erase_calls == 0U);
}

void testLegacyHistoryFailsBeforeAnyWriterIsMounted() {
  clearRegions();
  supplemental[0U] = 'H';
  supplemental[1U] = 'I';
  supplemental[2U] = 'S';
  supplemental[3U] = 'T';

  FlashMutationGate gate;
  k::SegmentedFlash segmented(gate.productPort(), gate,
      s::Role::kTracker, kTrackerId, true, true);
  assert(!segmented.begin());
  assert(write_calls == 0U && erase_calls == 0U);
  assert(segmented.program(k::kPrimaryBytes + kPageSize,
             "ABCD", 4U) == FlashOpResult::kFailed);
  assertOtherPartitionsUnchanged();
}

} // namespace

int main() {
  primary = mapAt(kProductRegionStart, k::kPrimaryBytes);
  geofence = mapAt(kGeofenceRegionStart, kGeofenceRegionPages * kPageSize);
  security = mapAt(kFutureSecurityRegionStart,
                   kFutureSecurityRegionPages * kPageSize);
  config = mapAt(kFutureConfigRegionStart,
                 kFutureConfigRegionPages * kPageSize);
  bond = mapAt(kFutureBondRegionStart,
               kFutureBondRegionPages * kPageSize);
  supplemental = mapAt(kBaseAddress, k::kSupplementalBytes);

  testExclusiveOwnerAndPhysicalBoundaries();
  testSharedSoftdeviceMutationSlot();
  testHistoryTimeoutQuarantineKeepsFlashOwned();
  testLegacyHistoryFailsBeforeAnyWriterIsMounted();

  assert(munmap(primary, k::kPrimaryBytes) == 0);
  assert(munmap(geofence, kGeofenceRegionPages * kPageSize) == 0);
  assert(munmap(security, kFutureSecurityRegionPages * kPageSize) == 0);
  assert(munmap(config, kFutureConfigRegionPages * kPageSize) == 0);
  assert(munmap(bond, kFutureBondRegionPages * kPageSize) == 0);
  assert(munmap(supplemental, k::kSupplementalBytes) == 0);
  puts("KQ05 REAL FlashMutationGate + 32/7 physical pages and async mutex: PASS (HOST ONLY)");
  puts("NO REAL NOR WRITES, NO HISTORY MIGRATION, NO DEVICE FLASH, NO ACK");
  return 0;
}
