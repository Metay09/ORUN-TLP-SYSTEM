// M7P6F M3 TEST-ONLY physical partial-erase sentinel.
//
// Destructive scope: ONLY the two-page SecurityStore partition
// 0x0E7000..0x0E9000. Never flash this image to a deployed/provisioned unit.
//
// Purpose:
// 1. Build the exact durable shape that exists after a higher-generation v2
//    page has been activated but before its superseded old page is erased.
// 2. Use the nRF52840 NVMC's real ERASEPAGEPARTIAL operation on the stale page
//    until a genuinely mixed (changed but not fully erased) physical page is
//    observed.
// 3. Append one valid higher TX bound to the intact new page as a reboot-phase
//    marker, reset, then run the production SecurityStore recovery code.
// 4. Accept only higher-generation PROVISIONED with non-rollback TX/A2D
//    evidence, or fail-closed FAULT/UNSUPPORTED.
//
// This is deterministic real-flash partial-erase evidence. It is NOT a claim
// that an external brownout/power yank itself has been electrically tested.
#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <nrf.h>
#include <nrf_sdm.h>

#include <string.h>

#include "device_identity.h"
#include "flash_backend.h"
#include "security_format.h"
#include "security_store.h"
#include "storage_config.h"

using namespace orun_tlp;
using namespace orun_tlp::security_format;
using namespace orun_tlp::storage_config;

namespace {

constexpr uint64_t kSentinelIdentity = 0x4D37503646334D33ULL;  // "M7P6F3M3"
constexpr uint32_t kSentinelEpoch = 1;
constexpr uint64_t kOldTxBound = 256;
constexpr uint64_t kOldA2dBound = 8;
constexpr uint64_t kNewTxBound = 512;
constexpr uint64_t kNewA2dBound = 16;
constexpr uint64_t kPostPartialMarkerTxBound = 768;

constexpr uint8_t kPartialDurationsMs[] = {5, 10, 20, 30, 40};

const uint8_t kCredentialId[kCredentialIdSize] = {
    0x4D, 0x37, 0x50, 0x36, 0x46, 0x2D, 0x4D, 0x33,
    0x2D, 0x50, 0x48, 0x59, 0x53, 0x2D, 0x30, 0x31,
};

const uint8_t kRoot[kKRootSize] = {
    0x53, 0x45, 0x4E, 0x54, 0x49, 0x4E, 0x45, 0x4C,
    0x2D, 0x54, 0x45, 0x53, 0x54, 0x2D, 0x4F, 0x4E,
    0x4C, 0x59, 0x2D, 0x4E, 0x4F, 0x54, 0x2D, 0x41,
    0x2D, 0x52, 0x45, 0x41, 0x4C, 0x2D, 0x4B, 0x45,
};

bool done = false;
uint32_t last_report_ms = 0;
char final_report[192] = "M7P6F M3 SENTINEL NOT RUN";

void setReport(const char* text) {
  strncpy(final_report, text, sizeof(final_report) - 1);
  final_report[sizeof(final_report) - 1] = '\0';
  Serial.println(final_report);
  Serial.flush();
  last_report_ms = millis();
  done = true;
}

bool softDeviceDisabled() {
  uint8_t enabled = 1;
  return sd_softdevice_is_enabled(&enabled) == NRF_SUCCESS && enabled == 0;
}

uint32_t pageBase(unsigned page) {
  return page * kPageSize;
}

bool programExact(FlashBackend& flash, uint32_t offset, const uint8_t* bytes,
                  size_t size) {
  return flash.program(offset, bytes, size) == FlashOpResult::kDone;
}

bool programCommitted(FlashBackend& flash, uint32_t offset,
                      const uint8_t* bytes, size_t size) {
  if (size < sizeof(uint32_t) || (size & 3U) != 0) return false;
  const size_t body_size = size - sizeof(uint32_t);
  return programExact(flash, offset, bytes, body_size) &&
         programExact(flash, offset + body_size, bytes + body_size,
                      sizeof(uint32_t));
}

Credential sentinelCredential() {
  Credential credential{};
  memcpy(credential.credential_id, kCredentialId, sizeof(kCredentialId));
  credential.key_epoch = kSentinelEpoch;
  credential.device_identity = kSentinelIdentity;
  memcpy(credential.k_root, kRoot, sizeof(kRoot));
  return credential;
}

SecurityStateRecord makeState(SecurityStateKind kind, uint64_t value) {
  SecurityStateRecord state{};
  memcpy(state.credential_id, kCredentialId, sizeof(kCredentialId));
  state.key_epoch = kSentinelEpoch;
  state.kind = kind;
  state.value = value;
  return state;
}

bool stageValidPage(FlashBackend& flash, unsigned page, uint64_t generation,
                    uint64_t tx_bound, uint64_t a2d_bound) {
  if (page >= kFutureSecurityRegionPages) return false;
  const uint32_t base = pageBase(page);

  uint8_t header[kPageHeaderSize];
  encodePageHeader(PageHeader(generation, kSentinelIdentity), header);
  // Activation word is deliberately withheld until every snapshot record is
  // committed, matching production's activation-last invariant.
  if (!programExact(flash, base + pageHeaderOffset(), header,
                    kPageHeaderSize - sizeof(uint32_t)))
    return false;

  uint8_t credential_bytes[kCredentialRecordSize];
  const Credential credential = sentinelCredential();
  encodeCredential(credential, credential_bytes);
  if (!programCommitted(flash, base + credentialRecordOffset(),
                        credential_bytes, sizeof(credential_bytes)))
    return false;

  uint8_t state_bytes[kSecurityStateRecordSize];
  SecurityStateRecord tx =
      makeState(SecurityStateKind::kTxReserveExclusiveBound, tx_bound);
  encodeSecurityState(tx, state_bytes);
  if (!programCommitted(flash, base + securityStateRecordOffset(0),
                        state_bytes, sizeof(state_bytes)))
    return false;

  SecurityStateRecord a2d =
      makeState(SecurityStateKind::kA2dReplayExclusiveBound, a2d_bound);
  encodeSecurityState(a2d, state_bytes);
  if (!programCommitted(flash, base + securityStateRecordOffset(1),
                        state_bytes, sizeof(state_bytes)))
    return false;

  return programExact(
      flash, base + pageHeaderOffset() + kPageHeaderSize - sizeof(uint32_t),
      header + kPageHeaderSize - sizeof(uint32_t), sizeof(uint32_t));
}

bool writePostPartialMarker(FlashBackend& flash) {
  uint8_t state_bytes[kSecurityStateRecordSize];
  const SecurityStateRecord marker =
      makeState(SecurityStateKind::kTxReserveExclusiveBound,
                kPostPartialMarkerTxBound);
  encodeSecurityState(marker, state_bytes);
  return programCommitted(
      flash, pageBase(1) + securityStateRecordOffset(2), state_bytes,
      sizeof(state_bytes));
}

bool postPartialMarkerPresent(const FlashBackend& flash) {
  uint8_t bytes[kSecurityStateRecordSize];
  if (!flash.read(pageBase(1) + securityStateRecordOffset(2), bytes,
                  sizeof(bytes)))
    return false;
  SecurityStateRecord state{};
  return decodeSecurityState(bytes, state) &&
         state.kind == SecurityStateKind::kTxReserveExclusiveBound &&
         state.key_epoch == kSentinelEpoch &&
         state.value == kPostPartialMarkerTxBound &&
         memcmp(state.credential_id, kCredentialId, sizeof(kCredentialId)) == 0;
}

bool waitNvmcReady() {
  for (uint32_t guard = 0; guard < 10000000UL; ++guard) {
    if (NRF_NVMC->READY == NVMC_READY_READY_Ready) return true;
  }
  return false;
}

bool partialEraseSecurityPage0(uint32_t duration_ms) {
  if (!softDeviceDisabled() || duration_ms == 0 || duration_ms >= 85U)
    return false;
  if (!waitNvmcReady()) return false;

  NRF_NVMC->CONFIG =
      (NVMC_CONFIG_WEN_Een << NVMC_CONFIG_WEN_Pos);
  if (!waitNvmcReady()) return false;

  NRF_NVMC->ERASEPAGEPARTIALCFG = duration_ms;
  NRF_NVMC->ERASEPAGEPARTIAL = kFutureSecurityRegionStart;
  if (!waitNvmcReady()) return false;

  NRF_NVMC->CONFIG =
      (NVMC_CONFIG_WEN_Ren << NVMC_CONFIG_WEN_Pos);
  return waitNvmcReady();
}

struct PartialEvidence {
  uint32_t changed_bytes = 0;
  uint32_t non_ff_bytes = 0;
  uint32_t illegal_one_to_zero_bytes = 0;
};

PartialEvidence comparePage(const uint8_t* before) {
  PartialEvidence out;
  const volatile uint8_t* after =
      reinterpret_cast<const volatile uint8_t*>(kFutureSecurityRegionStart);
  for (uint32_t i = 0; i < kPageSize; ++i) {
    const uint8_t old_value = before[i];
    const uint8_t new_value = after[i];
    if (new_value != old_value) ++out.changed_bytes;
    if (new_value != 0xFFU) ++out.non_ff_bytes;
    // Erase may only move programmed zero bits toward one. A new zero where
    // the staged page had a one is not a plausible erase transition.
    if ((static_cast<uint8_t>(new_value & old_value)) != old_value)
      ++out.illegal_one_to_zero_bytes;
  }
  return out;
}

bool settle(SecurityStore& store) {
  for (uint32_t guard = 0; guard < 1000 && store.busy(); ++guard)
    store.poll();
  return !store.busy();
}

void evaluateRecovery(NrfSecurityFlash& flash) {
  SecurityStore store(flash, flash);
  const DeviceIdentity identity =
      DeviceIdentity::fromLegacyUint64(kSentinelIdentity);
  if (!store.begin(identity)) {
    setReport("M7P6F M3 SENTINEL FAIL recovery_begin=false");
    return;
  }

  if (store.state() == SecurityState::kFault) {
    setReport("M7P6F M3 SENTINEL PASS outcome=FAULT fail_closed=yes");
    return;
  }
  if (store.state() == SecurityState::kUnsupported) {
    setReport("M7P6F M3 SENTINEL PASS outcome=UNSUPPORTED fail_closed=yes");
    return;
  }
  if (store.state() != SecurityState::kProvisioned) {
    char line[160];
    snprintf(line, sizeof(line),
             "M7P6F M3 SENTINEL FAIL unexpected_state=%u",
             static_cast<unsigned>(store.state()));
    setReport(line);
    return;
  }

  uint8_t recovered_id[kCredentialIdSize];
  if (!store.currentCredentialId(recovered_id) ||
      memcmp(recovered_id, kCredentialId, sizeof(kCredentialId)) != 0 ||
      store.currentKeyEpoch() != kSentinelEpoch) {
    setReport("M7P6F M3 SENTINEL FAIL provisioned_wrong_credential");
    return;
  }

  if (!settle(store)) {
    setReport("M7P6F M3 SENTINEL FAIL post_recovery_reserve_busy");
    return;
  }

  uint64_t tx = 0;
  uint32_t epoch = 0;
  const bool tx_ok = store.reserveNextTxCounter(tx, epoch) &&
                     epoch == kSentinelEpoch &&
                     tx >= kPostPartialMarkerTxBound;

  const bool replay_submitted = store.submitAuthenticatedA2dCounter(
      kCredentialId, kSentinelEpoch, kOldA2dBound);
  bool replay_accepted = true;
  const bool replay_result =
      replay_submitted && store.takeA2dReplayResult(replay_accepted);
  const bool replay_ok = replay_result && !replay_accepted;

  char line[192];
  snprintf(line, sizeof(line),
           "M7P6F M3 SENTINEL %s outcome=PROVISIONED tx=%llu epoch=%lu "
           "tx_nonrollback=%s a2d_old_reject=%s",
           (tx_ok && replay_ok) ? "PASS" : "FAIL",
           static_cast<unsigned long long>(tx),
           static_cast<unsigned long>(epoch), tx_ok ? "yes" : "no",
           replay_ok ? "yes" : "no");
  setReport(line);
}

void preparePartialEraseAndReset(NrfSecurityFlash& flash) {
  alignas(4) uint8_t before[kPageSize];

  for (uint8_t duration_ms : kPartialDurationsMs) {
    if (flash.erasePage(0) != FlashOpResult::kDone ||
        flash.erasePage(1) != FlashOpResult::kDone) {
      setReport("M7P6F M3 SENTINEL FAIL setup_erase");
      return;
    }
    if (!stageValidPage(flash, 0, 1, kOldTxBound, kOldA2dBound) ||
        !stageValidPage(flash, 1, 2, kNewTxBound, kNewA2dBound)) {
      setReport("M7P6F M3 SENTINEL FAIL setup_stage");
      return;
    }

    if (!flash.read(0, before, sizeof(before))) {
      setReport("M7P6F M3 SENTINEL FAIL snapshot_read");
      return;
    }

    if (!partialEraseSecurityPage0(duration_ms)) {
      setReport("M7P6F M3 SENTINEL FAIL nvmc_partial_erase");
      return;
    }

    const PartialEvidence evidence = comparePage(before);
    Serial.printf(
        "M7P6F M3 PARTIAL duration_ms=%u changed_bytes=%lu "
        "non_ff_bytes=%lu illegal_1_to_0_bytes=%lu\n",
        static_cast<unsigned>(duration_ms),
        static_cast<unsigned long>(evidence.changed_bytes),
        static_cast<unsigned long>(evidence.non_ff_bytes),
        static_cast<unsigned long>(evidence.illegal_one_to_zero_bytes));
    Serial.flush();

    const bool physically_mixed =
        evidence.changed_bytes > 0 && evidence.non_ff_bytes > 0 &&
        evidence.illegal_one_to_zero_bytes == 0;
    if (!physically_mixed) continue;

    if (!writePostPartialMarker(flash)) {
      setReport("M7P6F M3 SENTINEL FAIL marker_write");
      return;
    }

    Serial.printf(
        "M7P6F M3 PHYSICAL PARTIAL PASS duration_ms=%u "
        "changed_bytes=%lu non_ff_bytes=%lu; rebooting for production recovery\n",
        static_cast<unsigned>(duration_ms),
        static_cast<unsigned long>(evidence.changed_bytes),
        static_cast<unsigned long>(evidence.non_ff_bytes));
    Serial.flush();
    delay(250);
    NVIC_SystemReset();
    while (true) {}
  }

  setReport("M7P6F M3 SENTINEL FAIL no_mixed_partial_state_observed");
}

}  // namespace

void setup() {
  Serial.begin(115200);
  const uint32_t wait_started = millis();
  while (!Serial && (millis() - wait_started) < 15000U) delay(10);

  Serial.println(F("M7P6F M3 PARTIAL-ERASE SENTINEL BOOT"));
  Serial.println(F("TEST-ONLY: destructively owns ONLY SecurityStore 0xE7000..0xE9000"));
  Serial.flush();

  if (!softDeviceDisabled()) {
    setReport("M7P6F M3 SENTINEL FAIL softdevice_enabled");
    return;
  }

  NrfSecurityFlash flash;
  if (!flash.begin()) {
    setReport("M7P6F M3 SENTINEL FAIL security_flash_begin");
    return;
  }

  if (postPartialMarkerPresent(flash)) {
    Serial.println(F("M7P6F M3 phase=recovery-after-partial"));
    Serial.flush();
    evaluateRecovery(flash);
    return;
  }

  Serial.println(F("M7P6F M3 phase=prepare-real-partial-erase"));
  Serial.flush();
  preparePartialEraseAndReset(flash);
}

void loop() {
  if (done && Serial && (millis() - last_report_ms) >= 3000U) {
    Serial.println(final_report);
    Serial.flush();
    last_report_ms = millis();
  }
  delay(20);
}
