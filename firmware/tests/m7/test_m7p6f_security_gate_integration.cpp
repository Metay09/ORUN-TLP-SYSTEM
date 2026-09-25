// M7P6F audit regression: SecurityStore + the real FlashMutationGate async
// timeout/quarantine contract. The Nordic SVC boundary is faked, but both
// production state machines are real.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#include <vector>

#include <Arduino.h>
#include <nrf_sdm.h>

#include "flash_mutation_gate.h"
#include "security_store.h"
#include "storage_config.h"

using namespace orun_tlp;
using namespace orun_tlp::security_format;
using namespace orun_tlp::storage_config;

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

void fillId(uint8_t (&id)[kCredentialIdSize], uint8_t seed) {
  for (size_t i = 0; i < kCredentialIdSize; ++i)
    id[i] = static_cast<uint8_t>(seed + i);
}
void fillRoot(uint8_t (&root)[kKRootSize], uint8_t seed) {
  for (size_t i = 0; i < kKRootSize; ++i)
    root[i] = static_cast<uint8_t>(seed * 7 + i);
}

void settleSync(SecurityStore& store) {
  for (unsigned guard = 0; guard < 2000 && store.busy(); ++guard)
    store.poll();
  assert(!store.busy());
}

void completeAsync(FlashMutationGate& gate) {
  event_queue.push_back(NRF_EVT_FLASH_OPERATION_SUCCESS);
  gate.pumpEvents();
}
}  // namespace

namespace orun_tlp::monotonic {
uint32_t nowMs() { return fake_now_ms; }
}  // namespace orun_tlp::monotonic

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
  memset(reinterpret_cast<void*>(uintptr_t(page) * kPageSize), 0xFF,
         kPageSize);
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
  constexpr uint32_t kSecurityRegionSize =
      kFutureSecurityRegionEnd - kFutureSecurityRegionStart;
  void* security_region =
      mmap(reinterpret_cast<void*>(kFutureSecurityRegionStart),
           kSecurityRegionSize, PROT_READ | PROT_WRITE,
           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
  assert(security_region ==
         reinterpret_cast<void*>(kFutureSecurityRegionStart));
  memset(security_region, 0xFF, kSecurityRegionSize);

  // Build credential A with the synchronous boot path, exactly as production
  // does before Bluefruit enables SoftDevice.
  FlashMutationGate gate;
  SecurityStore store(gate.securityCriticalPort(),
                      gate.securityMaintPort());
  assert(store.begin(DeviceIdentity::fromLegacyUint64(
      0x0E8ADE7E71531AA3ULL)));

  uint8_t a_id[kCredentialIdSize], a_root[kKRootSize];
  fillId(a_id, 1);
  fillRoot(a_root, 1);
  assert(store.commitCredential(a_id, 1, a_root));
  settleSync(store);
  bool committed = false;
  assert(store.takeCommitResult(committed) && committed);
  assert(store.state() == SecurityState::kProvisioned);

  // Runtime BLE/SoftDevice is now active.
  sd_enabled = true;

  // First exercise an ordinary TX-reservation append whose BODY write is
  // physically accepted by SoftDevice but whose completion is withheld until
  // FlashMutationGate times out. SecurityStore must burn the dirty slot,
  // reject use of that reservation, survive the late SUCCESS, and continue at
  // the next slot without reprogramming the dirty target.
  for (uint64_t expected = 0; expected < kTxReservationBlockSize; ++expected) {
    uint64_t tx = 0;
    uint32_t epoch = 0;
    assert(store.reserveNextTxCounter(tx, epoch));
    assert(tx == expected);
    assert(epoch == 1);
  }

  store.poll();  // create the next reservation job.
  const unsigned writes_before_torn_append = write_calls;
  store.poll();  // submit body: fake SVC mutates, completion withheld.
  assert(write_calls == writes_before_torn_append + 1);

  fake_now_ms += 4001;
  store.poll();  // gate timeout -> dirty slot burned, reservation rejected.
  assert(gate.securityDiagnostics().timeouts == 1);
  assert(store.state() == SecurityState::kProvisioned);
  assert(store.diagnostics().reservation_failures == 1);

  completeAsync(gate);  // late body SUCCESS only reconciles quarantine.
  assert(gate.securityDiagnostics().late_completions == 2);

  // Retry uses the next slot. Complete body then commit normally.
  store.poll();  // create retry reservation job.
  store.poll();  // submit body.
  completeAsync(gate);
  store.poll();  // body completion + verification; submit commit.
  completeAsync(gate);
  store.poll();  // commit completion + final verification.
  assert(!store.busy());

  uint64_t resumed_tx = 0;
  uint32_t resumed_epoch = 0;
  assert(store.reserveNextTxCounter(resumed_tx, resumed_epoch));
  assert(resumed_tx == kTxReservationBlockSize);
  assert(resumed_epoch == 1);

  // Now begin credential B and drive every async operation through definitive
  // success until PAGE ACTIVATION.
  uint8_t b_id[kCredentialIdSize], b_root[kKRootSize];
  fillId(b_id, 2);
  fillRoot(b_root, 2);
  assert(store.commitCredential(b_id, 2, b_root));

  // Erase inactive page.
  store.poll();
  assert(erase_calls >= 2);  // includes synchronous page-A provisioning erase.
  completeAsync(gate);
  store.poll();

  // Header body.
  store.poll();
  completeAsync(gate);
  store.poll();

  // Credential body; completion causes commit-word submission in same poll.
  store.poll();
  completeAsync(gate);
  store.poll();

  // Credential commit.
  completeAsync(gate);
  store.poll();

  // Activation is accepted and physically mutates in the fake SVC, but its
  // completion event is deliberately withheld.
  const unsigned writes_before_activation = write_calls;
  store.poll();
  assert(write_calls == writes_before_activation + 1);

  fake_now_ms += 4001;
  store.poll();
  assert(gate.securityDiagnostics().timeouts == 2);
  assert(store.state() == SecurityState::kFault);
  assert(store.diagnostics().activation_ambiguities == 1);
  assert(store.takeCommitResult(committed) && !committed);

  uint64_t tx = 0;
  uint32_t epoch = 0;
  assert(!store.reserveNextTxCounter(tx, epoch));

  // Definitive late SUCCESS only reconciles the quarantined physical owner.
  // It must not resurrect the old RAM authority.
  completeAsync(gate);
  assert(gate.securityDiagnostics().late_completions == 1);
  assert(store.state() == SecurityState::kFault);

  // Simulated reboot: SoftDevice is disabled before store recovery. The
  // higher-generation B page that physically activated is now authoritative.
  sd_enabled = false;
  {
    FlashMutationGate reboot_gate;
    SecurityStore recovered(reboot_gate.securityCriticalPort(),
                            reboot_gate.securityMaintPort());
    assert(recovered.begin(DeviceIdentity::fromLegacyUint64(
        0x0E8ADE7E71531AA3ULL)));
    assert(recovered.state() == SecurityState::kProvisioned);
    uint8_t recovered_id[kCredentialIdSize];
    assert(recovered.currentCredentialId(recovered_id));
    assert(memcmp(recovered_id, b_id, kCredentialIdSize) == 0);
    assert(recovered.currentKeyEpoch() == 2);
  }

  assert(munmap(security_region, kSecurityRegionSize) == 0);
  puts("M7P6F SecurityStore/FlashMutationGate timeout integration: PASS");
}
