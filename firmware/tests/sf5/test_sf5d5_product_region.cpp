// SF5D5: the reserved 128 KiB product region, its nRF backend under
// FlashMutationGate, and the D2 one-owner-per-device decision. Host-only:
// the region is an mmap at its real address; Nordic SVCs are stubs.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#include <vector>

#include <Arduino.h>
#include <nrf_sdm.h>

#include "flash_mutation_gate.h"
#include "observation_store.h"
#include "product_region.h"
#include "storage_config.h"

using namespace orun_tlp;
using namespace orun_tlp::storage_config;
namespace osf = orun_tlp::observation_store_format;
namespace pr = orun_tlp::product_region;

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
}  // namespace

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

namespace {

constexpr uint32_t kProductSize = kProductRegionEnd - kProductRegionStart;
constexpr uint32_t kConfigSize =
    kFutureConfigRegionEnd - kFutureConfigRegionStart;
constexpr uint32_t kSecuritySize =
    kFutureSecurityRegionEnd - kFutureSecurityRegionStart;
constexpr uint32_t kGeofenceSize = kGeofenceRegionEnd - kGeofenceRegionStart;

static_assert(kProductRegionStart == 0x0C5000U, "SF5D5 region start");
static_assert(kProductSize == 128U * 1024U, "SF5D5 region size");
static_assert(kApplicationPolicyEndAddress == kProductRegionStart,
              "application ceiling is the product start");
static_assert(ObservationStore::kMaxPages >= kProductRegionPages,
              "ObservationStore must cover the whole region");

uint8_t* product = nullptr;
uint8_t* history = nullptr;
uint8_t* config = nullptr;
uint8_t* security = nullptr;
uint8_t* geofence = nullptr;

uint8_t* mapAt(uint32_t address, uint32_t size) {
  void* mapped = mmap(reinterpret_cast<void*>(address), size,
                      PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
  assert(mapped == reinterpret_cast<void*>(address));
  memset(mapped, 0xFF, size);
  return static_cast<uint8_t*>(mapped);
}

void eraseAll() {
  memset(product, 0xFF, kProductSize);
  memset(history, 0xFF, kRegionSize);
  memset(config, 0xFF, kConfigSize);
  memset(security, 0xFF, kSecuritySize);
  memset(geofence, 0xFF, kGeofenceSize);
}

bool allFf(const uint8_t* bytes, size_t size) {
  for (size_t i = 0; i < size; ++i)
    if (bytes[i] != 0xFF) return false;
  return true;
}

class FixedIncarnation : public ObservationIncarnationSource {
 public:
  bool generate(uint64_t& value) override {
    value = 0x5F5D5A5B5C5D5E5FULL;
    return true;
  }
};

// Runs the store to idle. In async mode every accepted SoftDevice operation
// completes on the next pump, as the SoC event would on the device.
void settle(ObservationStore& store, FlashMutationGate& gate) {
  for (unsigned i = 0; i < 2000U && store.busy(); ++i) {
    const unsigned before = write_calls + erase_calls;
    store.poll();
    if (sd_enabled && write_calls + erase_calls != before) {
      events.push_back(NRF_EVT_FLASH_OPERATION_SUCCESS);
      gate.pumpEvents();
    }
  }
  assert(!store.busy());
}

void prepareData(ObservationStore& store, FlashMutationGate& gate) {
  assert(store.requestMaintenance() ==
         ObservationStore::MaintenanceResult::kStarted);
  settle(store, gate);
  bool success = false;
  assert(store.takeMaintenanceResult(success) && success);
}

ObservationStore::Handle appendPeriodic(ObservationStore& store,
                                        FlashMutationGate& gate,
                                        uint8_t seed) {
  uint8_t payload[osf::kPeriodicPayloadSizeV1];
  for (unsigned i = 0; i < sizeof(payload); ++i)
    payload[i] = static_cast<uint8_t>(seed + i);
  assert(store.requestAppend(osf::RecordKind::kPeriodic, 1U, payload,
                             sizeof(payload)) ==
         ObservationStore::AppendResult::kStarted);
  settle(store, gate);
  bool success = false;
  ObservationStore::Handle handle;
  assert(store.takeAppendResult(success, handle) && success);
  return handle;
}

// 1. Synchronous backend: exact region, bounds, alignment, page crossing,
// program size, no reprogram without erase, erase readback.
void testSyncBounds() {
  resetHarness();
  eraseAll();
  FlashMutationGate gate;
  FlashBackend& port = gate.productPort();
  assert(port.begin());

  alignas(4) uint8_t body[144];
  for (unsigned i = 0; i < sizeof(body); ++i) body[i] = uint8_t(i + 3U);

  // The largest real write (control body+CRC, 132 bytes) and a whole slot.
  assert(port.program(0, body, 132) == FlashOpResult::kDone);
  assert(memcmp(product, body, 132) == 0);
  assert(port.program(4096, body, 144) == FlashOpResult::kDone);
  assert(memcmp(product + 4096, body, 144) == 0);
  alignas(4) uint8_t big[148]{};
  assert(port.program(8192, big, sizeof(big)) == FlashOpResult::kFailed);

  // Last word of the region maps to 0x0E4FFC, the geofence region untouched.
  assert(port.program(kProductSize - 4U, body, 4) == FlashOpResult::kDone);
  assert(memcmp(reinterpret_cast<const uint8_t*>(kProductRegionEnd - 4U),
                body, 4) == 0);
  assert(allFf(geofence, kGeofenceSize));
  assert(port.program(kProductSize - 4U, body, 8) == FlashOpResult::kFailed);
  assert(port.program(kProductSize, body, 4) == FlashOpResult::kFailed);

  // Alignment and page crossing.
  assert(port.program(12288 + 2U, body, 4) == FlashOpResult::kFailed);
  assert(port.program(12288, body, 6) == FlashOpResult::kFailed);
  assert(port.program(12288 - 4U, body, 8) == FlashOpResult::kFailed);

  // NOR flash: an already programmed word is never programmed again.
  const unsigned before = write_calls;
  assert(port.program(0, body, 4) == FlashOpResult::kFailed);
  assert(write_calls == before);

  // Reads stay inside the region.
  uint8_t out[8];
  assert(port.read(kProductSize - 8U, out, 8));
  assert(!port.read(kProductSize - 4U, out, 8));

  // Erase: only the 32 owned pages, verified all-FF afterwards.
  assert(port.erasePage(kProductRegionPages) == FlashOpResult::kFailed);
  assert(port.erasePage(0) == FlashOpResult::kDone);
  assert(allFf(product, kPageSize));
  assert(port.erasePage(kProductRegionPages - 1U) == FlashOpResult::kDone);
  assert(allFf(product + kProductSize - kPageSize, kPageSize));
  // A rejected erase is reported, never trusted.
  assert(port.program(2U * kPageSize, body, 4) == FlashOpResult::kDone);
  submit_results = {NRF_ERROR_INTERNAL};
  assert(port.erasePage(2) == FlashOpResult::kFailed);
  assert(memcmp(product + 2U * kPageSize, body, 4) == 0);
  assert(gate.productDiagnostics().submits >= 1U);
}

// 2. Asynchronous path: History > Product > Config. Hold SEC_MAINT admitted
// (BUSY) so the others can stage behind it.
void testAdmissionOrder() {
  resetHarness();
  eraseAll();
  FlashMutationGate gate;
  assert(gate.begin());
  assert(gate.productPort().begin());
  assert(gate.configPort().begin());
  assert(gate.securityMaintPort().begin());
  sd_enabled = true;

  // Product outranks Config.
  submit_results = {NRF_ERROR_BUSY};
  assert(gate.securityMaintPort().erasePage(0) == FlashOpResult::kPending);
  alignas(4) uint8_t c[4] = {1, 2, 3, 4};
  alignas(4) uint8_t p[4] = {5, 6, 7, 8};
  assert(gate.configPort().program(0, c, 4) == FlashOpResult::kPending);
  assert(gate.productPort().program(0, p, 4) == FlashOpResult::kPending);
  submit_results.clear();
  assert(gate.securityMaintPort().pollPending() == FlashOpResult::kPending);
  events.push_back(NRF_EVT_FLASH_OPERATION_SUCCESS);
  gate.pumpEvents();
  assert(gate.securityMaintPort().pollPending() == FlashOpResult::kDone);

  unsigned before = write_calls;
  assert(gate.configPort().pollPending() == FlashOpResult::kPending);
  assert(write_calls == before);  // Product is staged with higher priority.
  assert(gate.productPort().pollPending() == FlashOpResult::kPending);
  assert(write_calls == before + 1U);
  events.push_back(NRF_EVT_FLASH_OPERATION_SUCCESS);
  gate.pumpEvents();
  assert(gate.productPort().pollPending() == FlashOpResult::kDone);
  assert(memcmp(product, p, 4) == 0);
  assert(gate.configPort().pollPending() == FlashOpResult::kPending);
  events.push_back(NRF_EVT_FLASH_OPERATION_SUCCESS);
  gate.pumpEvents();
  assert(gate.configPort().pollPending() == FlashOpResult::kDone);

  // History outranks Product.
  submit_results = {NRF_ERROR_BUSY};
  assert(gate.securityMaintPort().erasePage(1) == FlashOpResult::kPending);
  alignas(4) uint8_t h[4] = {9, 9, 9, 9};
  assert(gate.productPort().program(4, p, 4) == FlashOpResult::kPending);
  assert(gate.program(0, h, 4) == FlashOpResult::kPending);
  submit_results.clear();
  assert(gate.securityMaintPort().pollPending() == FlashOpResult::kPending);
  events.push_back(NRF_EVT_FLASH_OPERATION_SUCCESS);
  gate.pumpEvents();
  assert(gate.securityMaintPort().pollPending() == FlashOpResult::kDone);

  before = write_calls;
  assert(gate.productPort().pollPending() == FlashOpResult::kPending);
  assert(write_calls == before);  // History waits with higher priority.
  assert(gate.pollPending() == FlashOpResult::kPending);
  assert(write_calls == before + 1U);
  events.push_back(NRF_EVT_FLASH_OPERATION_SUCCESS);
  gate.pumpEvents();
  assert(gate.pollPending() == FlashOpResult::kDone);
  assert(gate.productPort().pollPending() == FlashOpResult::kPending);
  events.push_back(NRF_EVT_FLASH_OPERATION_SUCCESS);
  gate.pumpEvents();
  assert(gate.productPort().pollPending() == FlashOpResult::kDone);
  assert(memcmp(product + 4, p, 4) == 0);
}

// 3. The full 132-byte write is staged inside the gate before async
// submission; a caller changing its buffer afterwards changes nothing.
// 4. An accepted request that times out quarantines and is reconciled only
// by the late completion event.
void testStagingAndQuarantine() {
  resetHarness();
  eraseAll();
  FlashMutationGate gate;
  assert(gate.productPort().begin());
  sd_enabled = true;

  alignas(4) uint8_t body[132];
  uint8_t expected[132];
  for (unsigned i = 0; i < sizeof(body); ++i)
    body[i] = expected[i] = uint8_t(i ^ 0x5AU);
  assert(gate.productPort().program(8192, body, sizeof(body)) ==
         FlashOpResult::kPending);
  memset(body, 0, sizeof(body));
  events.push_back(NRF_EVT_FLASH_OPERATION_SUCCESS);
  gate.pumpEvents();
  assert(gate.productPort().pollPending() == FlashOpResult::kDone);
  assert(memcmp(product + 8192, expected, sizeof(expected)) == 0);

  alignas(4) uint8_t word[4] = {1, 1, 1, 1};
  assert(gate.productPort().program(12288, word, 4) == FlashOpResult::kPending);
  fake_now_ms = 4001;
  assert(gate.productPort().pollPending() == FlashOpResult::kFailed);
  assert(gate.productMutationUnreconciled());
  assert(gate.productPort().hasUnreconciledMutation());
  assert(gate.productDiagnostics().timeouts == 1U);
  // Nothing new is accepted while the outcome is unknown.
  assert(gate.productPort().program(12292, word, 4) == FlashOpResult::kFailed);
  events.push_back(NRF_EVT_FLASH_OPERATION_SUCCESS);
  gate.pumpEvents();
  assert(!gate.productMutationUnreconciled());
  assert(gate.productDiagnostics().late_completions == 1U);
}

// 5. ObservationStore over the real 32-page product port: append, release,
// remount. Once with SoftDevice off, once through the async gate path.
void testObservationStoreOnRegion(bool async) {
  resetHarness();
  eraseAll();
  const uint64_t device_id = 0x0E8ADE7E71531AA3ULL;
  ObservationStore::Handle first;
  {
    FlashMutationGate gate;
    FixedIncarnation source;
    ObservationStore store(gate.productPort(), kProductRegionPages, &source);
    // Like every other store, it mounts before SoftDevice starts (the
    // synchronous backend's begin() refuses once SoftDevice is enabled).
    assert(store.begin(device_id));
    assert(store.ready() && !store.faulted());
    sd_enabled = async;
    assert(store.dataPageCount() == kProductRegionPages - 2U);
    prepareData(store, gate);
    first = appendPeriodic(store, gate, 10U);
    (void)appendPeriodic(store, gate, 20U);
    (void)appendPeriodic(store, gate, 30U);
    assert(store.requestRelease(first.identity));
    settle(store, gate);
    bool success = false;
    assert(store.takeReleaseResult(success) && success);
    uint32_t retained = 0;
    assert(store.retainedCount(retained) && retained == 2U);
    if (async) {
      assert(gate.productDiagnostics().async_accepted > 0U);
      assert(gate.productDiagnostics().completions_error == 0U);
    }
  }
  assert(allFf(geofence, kGeofenceSize));
  sd_enabled = false;  // reboot: SoftDevice starts later again
  {
    FlashMutationGate gate;
    assert(gate.productPort().begin());
    FixedIncarnation source;
    ObservationStore store(gate.productPort(), kProductRegionPages, &source);
    assert(store.begin(device_id));
    assert(store.ready() && !store.faulted());
    uint32_t retained = 0;
    assert(store.retainedCount(retained) && retained == 2U);
    ObservationStore::Record record;
    assert(store.oldestRetained(record) ==
           ObservationStore::LookupResult::kFound);
    assert(record.payload[0] == 20U);

    pr::Census census{};
    assert(pr::takeCensus(gate.productPort(), kProductRegionPages, census));
    assert(census.observation == 1U && census.custody == 0U &&
           census.unknown == 0U);
    assert(census.observation + census.erased == kProductRegionPages);
    assert(pr::mountDecision(pr::Owner::kObservation, census) ==
           pr::MountDecision::kMount);
    // The same region on a device with the Gateway job is refused, never
    // reformatted.
    assert(pr::mountDecision(pr::Owner::kCustody, census) ==
           pr::MountDecision::kForeignStore);
  }
}

// 5b. Fill the region until the store has used every data page and wrapped
// once: all 32 pages stay inside [0x0C5000, 0x0E5000).
void testFillWholeRegion() {
  resetHarness();
  eraseAll();
  FlashMutationGate gate;
  FixedIncarnation source;
  ObservationStore store(gate.productPort(), kProductRegionPages, &source);
  assert(store.begin(0x1234U));
  const uint32_t data_records =
      uint32_t(kProductRegionPages - 2U) * osf::kDataRecordsPerPage;
  uint32_t appended = 0;
  for (unsigned guard = 0; guard < 4000U && appended < data_records + 50U;
       ++guard) {
    uint8_t payload[osf::kPeriodicPayloadSizeV1];
    memset(payload, uint8_t(appended), sizeof(payload));
    const ObservationStore::AppendResult result = store.requestAppend(
        osf::RecordKind::kPeriodic, 1U, payload, sizeof(payload));
    if (result == ObservationStore::AppendResult::kStarted) {
      settle(store, gate);
      bool success = false;
      ObservationStore::Handle handle;
      assert(store.takeAppendResult(success, handle) && success);
      ++appended;
      continue;
    }
    assert(result == ObservationStore::AppendResult::kNoCapacity);
    const ObservationStore::MaintenanceResult maintenance =
        store.requestMaintenance();
    bool success = false;
    if (maintenance == ObservationStore::MaintenanceResult::kStarted) {
      settle(store, gate);
      assert(store.takeMaintenanceResult(success) && success);
    } else {
      assert(maintenance ==
             ObservationStore::MaintenanceResult::kControlMaintenanceRequired);
      assert(store.requestControlMaintenance() ==
             ObservationStore::MaintenanceResult::kStarted);
      settle(store, gate);
      assert(store.takeControlMaintenanceResult(success) && success);
    }
  }
  assert(appended == data_records + 50U);
  assert(!store.faulted());
  assert(store.diagnostics().pages_reclaimed >= 1U);
  assert(store.diagnostics().capacity_lost_total > 0U);

  pr::Census census{};
  assert(pr::takeCensus(gate.productPort(), kProductRegionPages, census));
  assert(census.unknown == 0U && census.custody == 0U);
  assert(census.observation >= kProductRegionPages - 2U);
  // The highest page of the region carries a store page, nothing beyond it.
  assert(!allFf(product + kProductSize - kPageSize, kPageSize));
  assert(allFf(geofence, kGeofenceSize));
}

// 6. D2 owner by job and the mount decision on leftovers and foreign pages.
void testOwnerAndMountDecision() {
  assert(pr::ownerFor(pr::Jobs{false, false}) == pr::Owner::kNone);
  assert(pr::ownerFor(pr::Jobs{false, true}) == pr::Owner::kObservation);
  assert(pr::ownerFor(pr::Jobs{true, false}) == pr::Owner::kCustody);
  // A gateway that could also track still keeps the region for custody.
  assert(pr::ownerFor(pr::Jobs{true, true}) == pr::Owner::kCustody);
  assert(strcmp(pr::ownerName(pr::Owner::kCustody), "CUSTODY") == 0);

  resetHarness();
  eraseAll();
  FlashMutationGate gate;
  FlashBackend& port = gate.productPort();
  assert(port.begin());

  pr::Census census{};
  assert(pr::takeCensus(port, kProductRegionPages, census));
  assert(census.erased == kProductRegionPages);
  assert(pr::mountDecision(pr::Owner::kObservation, census) ==
         pr::MountDecision::kMount);
  assert(pr::mountDecision(pr::Owner::kCustody, census) ==
         pr::MountDecision::kMount);
  assert(pr::mountDecision(pr::Owner::kNone, census) ==
         pr::MountDecision::kNoOwner);

  // Leftovers from old images / SF5D4 markers: a byte deep inside a page and
  // a marker at a page start are both "unknown".
  product[5U * kPageSize + 4000U] = 0x12;
  alignas(4) const uint8_t marker[4] = {'S', 'F', '5', 'D'};
  assert(port.program(6U * kPageSize, marker, 4) == FlashOpResult::kDone);
  assert(pr::takeCensus(port, kProductRegionPages, census));
  assert(census.unknown == 2U && census.erased == kProductRegionPages - 2U);
  assert(pr::mountDecision(pr::Owner::kObservation, census) ==
         pr::MountDecision::kNeedsPreparation);
  assert(strcmp(pr::mountDecisionName(pr::MountDecision::kNeedsPreparation),
                "NEEDS_PREPARATION") == 0);

  // A page starting with the CustodyStore magic belongs to the other store.
  alignas(4) const uint8_t custody_magic[4] = {'O', 'C', 'Q', '1'};
  assert(port.program(9U * kPageSize, custody_magic, 4) ==
         FlashOpResult::kDone);
  assert(pr::takeCensus(port, kProductRegionPages, census));
  assert(census.custody == 1U && census.unknown == 2U);
  assert(pr::mountDecision(pr::Owner::kObservation, census) ==
         pr::MountDecision::kForeignStore);
  // The custody owner sees its own page among leftovers: mount, and the
  // store's own recovery decides (and fails closed) on the rest.
  assert(pr::mountDecision(pr::Owner::kCustody, census) ==
         pr::MountDecision::kMount);
}

}  // namespace

int main() {
  product = mapAt(kProductRegionStart, kProductSize);
  geofence = mapAt(kGeofenceRegionStart, kGeofenceSize);
  security = mapAt(kFutureSecurityRegionStart, kSecuritySize);
  config = mapAt(kFutureConfigRegionStart, kConfigSize);
  history = mapAt(kBaseAddress, kRegionSize);

  testSyncBounds();
  testAdmissionOrder();
  testStagingAndQuarantine();
  testObservationStoreOnRegion(false);
  testObservationStoreOnRegion(true);
  testFillWholeRegion();
  testOwnerAndMountDecision();

  assert(munmap(product, kProductSize) == 0);
  assert(munmap(geofence, kGeofenceSize) == 0);
  assert(munmap(security, kSecuritySize) == 0);
  assert(munmap(config, kConfigSize) == 0);
  assert(munmap(history, kRegionSize) == 0);
  puts("SF5D5 product region, backend, gate client and owner checks: PASS");
}
