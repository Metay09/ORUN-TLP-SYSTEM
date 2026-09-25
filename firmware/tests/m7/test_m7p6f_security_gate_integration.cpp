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

  uint8_t a_id[kCredentialIdSize], a_root[kKRootSize];
  uint8_t b_id[kCredentialIdSize], b_root[kKRootSize];
  fillId(a_id, 1);
  fillRoot(a_root, 1);
  fillId(b_id, 2);
  fillRoot(b_root, 2);

  // Scenario 1: an ordinary append BODY is physically accepted but its
  // completion is withheld until the real FlashMutationGate times out. This
  // is an unreconciled accepted mutation, not an ordinary clean failure.
  // Product policy is deliberately fail-closed until reboot: one such timeout
  // faults SecurityStore immediately, repeated loop polls must not manufacture
  // extra failures, and a late SUCCESS may release the physical gate but may
  // not revive same-boot security authority.
  {
    FlashMutationGate gate;
    SecurityStore store(gate.securityCriticalPort(),
                        gate.securityMaintPort());
    assert(store.begin(DeviceIdentity::fromLegacyUint64(
        0x0E8ADE7E71531AA3ULL)));
    assert(store.commitCredential(a_id, 1, a_root));
    settleSync(store);
    bool committed = false;
    assert(store.takeCommitResult(committed) && committed);
    assert(store.state() == SecurityState::kProvisioned);

    sd_enabled = true;
    for (uint64_t expected = 0;
         expected < kTxReservationBlockSize; ++expected) {
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
    store.poll();  // timeout -> quarantine + explicit SecurityStore FAULT.
    assert(gate.securityDiagnostics().timeouts == 1);
    assert(store.state() == SecurityState::kFault);
    assert(store.diagnostics().reservation_failures == 1);
    assert(store.diagnostics().unreconciled_mutation_faults == 1);

    const unsigned writes_at_fault = write_calls;
    const unsigned erases_at_fault = erase_calls;
    const uint32_t reservation_failures_at_fault =
        store.diagnostics().reservation_failures;
    for (unsigned pass = 0; pass < 5; ++pass) store.poll();
    assert(store.state() == SecurityState::kFault);
    assert(write_calls == writes_at_fault);
    assert(erase_calls == erases_at_fault);
    assert(store.diagnostics().reservation_failures ==
           reservation_failures_at_fault);
    assert(store.diagnostics().mutation_failure_lockouts == 0);

    completeAsync(gate);
    assert(gate.securityDiagnostics().late_completions == 1);
    assert(store.state() == SecurityState::kFault);
    assert(!store.busy());
    uint64_t tx = 0;
    uint32_t epoch = 0;
    assert(!store.reserveNextTxCounter(tx, epoch));
  }

  // Scenario 2 starts exactly as a real reboot does: SoftDevice is disabled
  // during SecurityStore recovery. The timed-out append body had no commit
  // word, so recovery burns that slot, keeps credential A authoritative and
  // can reserve fresh TX space. Then exercise the existing activation
  // ambiguity case independently under the same explicit timeout policy.
  sd_enabled = false;
  {
    FlashMutationGate gate;
    SecurityStore store(gate.securityCriticalPort(),
                        gate.securityMaintPort());
    assert(store.begin(DeviceIdentity::fromLegacyUint64(
        0x0E8ADE7E71531AA3ULL)));
    assert(store.state() == SecurityState::kProvisioned);
    assert(store.diagnostics().recovery_burned_slots >= 1);
    settleSync(store);

    uint8_t recovered_a_id[kCredentialIdSize];
    assert(store.currentCredentialId(recovered_a_id));
    assert(memcmp(recovered_a_id, a_id, kCredentialIdSize) == 0);

    // The timed-out body never committed a new TX bound. Reboot must burn
    // the prior reserved headroom and resume at or above the next block,
    // never reissue a counter from the pre-timeout lifetime.
    uint64_t recovered_tx = 0;
    uint32_t recovered_epoch = 0;
    assert(store.reserveNextTxCounter(recovered_tx, recovered_epoch));
    assert(recovered_tx >= kTxReservationBlockSize);
    assert(recovered_epoch == 1);

    sd_enabled = true;
    bool committed = false;
    assert(store.commitCredential(b_id, 2, b_root));

    // Erase inactive page.
    store.poll();
    completeAsync(gate);
    store.poll();

    // Header body.
    store.poll();
    completeAsync(gate);
    store.poll();

    // Credential body, then credential commit.
    store.poll();
    completeAsync(gate);
    store.poll();
    completeAsync(gate);
    store.poll();

    // Page activation itself physically mutates, but completion is withheld.
    const unsigned writes_before_activation = write_calls;
    store.poll();
    assert(write_calls == writes_before_activation + 1);

    fake_now_ms += 4001;
    store.poll();
    assert(gate.securityDiagnostics().timeouts == 1);
    assert(store.state() == SecurityState::kFault);
    assert(store.diagnostics().activation_ambiguities == 1);
    assert(store.takeCommitResult(committed) && !committed);

    uint64_t tx = 0;
    uint32_t epoch = 0;
    assert(!store.reserveNextTxCounter(tx, epoch));

    completeAsync(gate);
    assert(gate.securityDiagnostics().late_completions == 1);
    assert(store.state() == SecurityState::kFault);
  }

  // Final reboot: the physically activated higher-generation B page wins.
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
