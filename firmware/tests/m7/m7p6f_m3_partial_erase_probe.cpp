// M7P6F M3 TEST-ONLY physical interrupted-erase sentinel.
//
// Destructive scope: ONLY the two-page SecurityStore partition
// 0x0E7000..0x0E9000. Never flash this image to a deployed/provisioned unit.
//
// Purpose:
// 1. Build the exact durable shape that exists after a higher-generation v2
//    page has been activated but before its superseded old page is erased.
// 2. Commit a monotonic marker to the intact new page, arm the nRF52840
//    hardware watchdog for ~1 ms, then start a real full-page NVMC erase of
//    the stale page. Flash execution stalls the CPU while peripherals keep
//    running, so a DOG reset proves reset occurred before the erase-return
//    instruction could execute. If erase returns first, software reset is
//    requested immediately and the next boot rejects the run as unproven.
// 3. On a proven DOG reboot, run the production SecurityStore recovery code.
// 4. Accept only higher-generation PROVISIONED with non-rollback TX/A2D
//    evidence, or fail-closed FAULT/UNSUPPORTED.
//
// This is deterministic real-flash reset-during-erase evidence. It is NOT a
// claim that an external electrical brownout or power yank was reproduced.
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
bool run_started = false;
uint32_t last_report_ms = 0;
uint32_t last_ready_report_ms = 0;
char command_buffer[16]{};
uint8_t command_length = 0;
NrfSecurityFlash sentinel_flash;
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
                    uint64_t tx_bound, uint64_t a2d_bound,
                    bool dense_old_page = false) {
  if (page >= kFutureSecurityRegionPages) return false;
  const uint32_t base = pageBase(page);

  uint8_t header[security_format::kPageHeaderSize];
  encodePageHeader(PageHeader(generation, kSentinelIdentity), header);
  // Activation word is deliberately withheld until every snapshot record is
  // committed, matching production's activation-last invariant.
  if (!programExact(flash, base + pageHeaderOffset(), header,
                    security_format::kPageHeaderSize - sizeof(uint32_t)))
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

  if (dense_old_page) {
    // A real NVMC partial erase may progress through the page while our normal
    // compact snapshot programs only the first ~180 bytes. With the sparse
    // image, even a short partial erase can erase every programmed byte and
    // become observationally indistinguishable from a full-page erase.
    //
    // Fill the stale page with additional *valid* v2 state records spanning
    // the whole append area so a partial erase has programmed evidence across
    // nearly the entire 4 KiB page. Equal per-kind bounds are legal recovery
    // input (only decreases are rollback/corruption), so this keeps the stale
    // page structurally valid without raising its authority above the newer
    // page.
    for (unsigned slot = 2; slot < kSecurityStateSlotsPerPage; ++slot) {
      const bool tx_slot = (slot & 1U) == 0U;
      SecurityStateRecord filler = makeState(
          tx_slot ? SecurityStateKind::kTxReserveExclusiveBound
                  : SecurityStateKind::kA2dReplayExclusiveBound,
          tx_slot ? tx_bound : a2d_bound);
      encodeSecurityState(filler, state_bytes);
      if (!programCommitted(flash, base + securityStateRecordOffset(slot),
                            state_bytes, sizeof(state_bytes)))
        return false;
    }
  }

  return programExact(
      flash, base + pageHeaderOffset() + security_format::kPageHeaderSize - sizeof(uint32_t),
      header + security_format::kPageHeaderSize - sizeof(uint32_t), sizeof(uint32_t));
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

[[noreturn]] void startWatchdogInterruptedErase() {
  // ~0.98 ms: timeout = (CRV + 1) / 32768 s.
  constexpr uint32_t kWatchdogTicks = 32;

  if (NRF_WDT->RUNSTATUS != 0) {
    setReport("M7P6F M3 SENTINEL FAIL watchdog_already_running");
    while (true) delay(1000);
  }

  // Put NVMC into erase mode before starting the short watchdog. After
  // TASKS_START there must be no wait/Serial/flash helper between watchdog
  // arming and ERASEPAGE, otherwise a DOG reset would not prove that erase
  // itself had begun.
  if (!waitNvmcReady()) {
    setReport("M7P6F M3 SENTINEL FAIL nvmc_not_ready");
    while (true) delay(1000);
  }
  NRF_NVMC->CONFIG =
      (NVMC_CONFIG_WEN_Een << NVMC_CONFIG_WEN_Pos);
  if (!waitNvmcReady()) {
    setReport("M7P6F M3 SENTINEL FAIL nvmc_erase_enable");
    while (true) delay(1000);
  }

  // Prevent a USB/RTOS callback from consuming the short watchdog window
  // between WDT start and ERASEPAGE. No critical-section exit is needed
  // because this TEST-ONLY path intentionally ends in reset.
  taskENTER_CRITICAL();

  NRF_WDT->CONFIG =
      (WDT_CONFIG_SLEEP_Run << WDT_CONFIG_SLEEP_Pos) |
      (WDT_CONFIG_HALT_Run << WDT_CONFIG_HALT_Pos);
  NRF_WDT->CRV = kWatchdogTicks - 1U;
  NRF_WDT->RREN = (WDT_RREN_RR0_Enabled << WDT_RREN_RR0_Pos);
  NRF_WDT->TASKS_START = 1;

  // The next instruction fetch from flash cannot execute until ERASEPAGE
  // finishes. Therefore, with callbacks/scheduling masked:
  // - DOG without SREQ => watchdog reset occurred before erase returned;
  // - SREQ => erase returned first and the fallback software reset executed.
  NRF_NVMC->ERASEPAGE = kFutureSecurityRegionStart;
  NVIC_SystemReset();

  while (true) {}
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

  const uint32_t tx_hi = static_cast<uint32_t>(tx >> 32U);
  const uint32_t tx_lo = static_cast<uint32_t>(tx & 0xFFFFFFFFULL);
  char line[224];
  snprintf(line, sizeof(line),
           "M7P6F M3 SENTINEL %s outcome=PROVISIONED "
           "tx_hi=%lu tx_lo=%lu epoch=%lu "
           "tx_nonrollback=%s a2d_old_reject=%s",
           (tx_ok && replay_ok) ? "PASS" : "FAIL",
           static_cast<unsigned long>(tx_hi),
           static_cast<unsigned long>(tx_lo),
           static_cast<unsigned long>(epoch), tx_ok ? "yes" : "no",
           replay_ok ? "yes" : "no");
  setReport(line);
}

void preparePartialEraseAndReset(NrfSecurityFlash& flash) {
  if (flash.erasePage(0) != FlashOpResult::kDone ||
      flash.erasePage(1) != FlashOpResult::kDone) {
    setReport("M7P6F M3 SENTINEL FAIL setup_erase");
    return;
  }

  if (!stageValidPage(flash, 0, 1, kOldTxBound, kOldA2dBound, true) ||
      !stageValidPage(flash, 1, 2, kNewTxBound, kNewA2dBound)) {
    setReport("M7P6F M3 SENTINEL FAIL setup_stage");
    return;
  }

  if (!writePostPartialMarker(flash) || !postPartialMarkerPresent(flash)) {
    setReport("M7P6F M3 SENTINEL FAIL marker_write_verify");
    return;
  }

  Serial.println(
      F("M7P6F M3 INTERRUPT armed marker_tx=768 wdt_ms~1; starting old-page erase"));
  Serial.flush();
  delay(100);

  startWatchdogInterruptedErase();
}


}  // namespace

void setup() {
  Serial.begin(115200);
  const uint32_t wait_started = millis();
  while (!Serial && (millis() - wait_started) < 15000U) delay(10);

  Serial.println(F("M7P6F M3 INTERRUPTED-ERASE SENTINEL BOOT"));
  Serial.println(F("TEST-ONLY: destructively owns ONLY SecurityStore 0xE7000..0xE9000"));
  Serial.flush();

  if (!softDeviceDisabled()) {
    setReport("M7P6F M3 SENTINEL FAIL softdevice_enabled");
    return;
  }

  if (!sentinel_flash.begin()) {
    setReport("M7P6F M3 SENTINEL FAIL security_flash_begin");
    return;
  }

  // A committed marker means the prior run armed the destructive old-page
  // erase. Require the saved reset reason to prove the watchdog, not the
  // software fallback immediately after ERASEPAGE, caused the reboot.
  if (postPartialMarkerPresent(sentinel_flash)) {
    const uint32_t reset_reason = readResetReason();
    Serial.printf("M7P6F M3 phase=recovery-after-interrupted-erase reset_reason=0x%08lX\n",
                  static_cast<unsigned long>(reset_reason));
    Serial.flush();

    const bool watchdog_reset =
        (reset_reason & POWER_RESETREAS_DOG_Msk) != 0U;
    const bool software_reset =
        (reset_reason & POWER_RESETREAS_SREQ_Msk) != 0U;
    if (!watchdog_reset || software_reset) {
      char line[160];
      snprintf(line, sizeof(line),
               "M7P6F M3 SENTINEL FAIL interruption_not_proven reset_reason=0x%08lX",
               static_cast<unsigned long>(reset_reason));
      setReport(line);
      return;
    }

    evaluateRecovery(sentinel_flash);
    return;
  }

  // Do not auto-start destructive flash work immediately after DFU. Require
  // an explicit operator RUN command on a blank/no-marker boot so the monitor
  // is attached before the watchdog-interrupted erase begins.
  Serial.println(F("M7P6F M3 READY send RUN"));
  Serial.flush();
  last_ready_report_ms = millis();
}

void loop() {
  if (done) {
    if (Serial && (millis() - last_report_ms) >= 3000U) {
      Serial.println(final_report);
      Serial.flush();
      last_report_ms = millis();
    }
    delay(20);
    return;
  }

  if (!run_started) {
    if (Serial && (millis() - last_ready_report_ms) >= 3000U) {
      Serial.println(F("M7P6F M3 READY send RUN"));
      Serial.flush();
      last_ready_report_ms = millis();
    }

    while (Serial.available() > 0) {
      const char ch = static_cast<char>(Serial.read());
      if (ch == '\r') continue;
      if (ch == '\n') {
        command_buffer[command_length] = '\0';
        if (strcmp(command_buffer, "RUN") == 0) {
          run_started = true;
          Serial.println(F("M7P6F M3 RUN accepted"));
          Serial.flush();
          preparePartialEraseAndReset(sentinel_flash);
        } else if (command_length != 0) {
          Serial.println(F("M7P6F M3 command rejected; send RUN"));
          Serial.flush();
        }
        command_length = 0;
        break;
      }
      if (command_length + 1U < sizeof(command_buffer)) {
        command_buffer[command_length++] = ch;
      } else {
        command_length = 0;
        Serial.println(F("M7P6F M3 command too long; send RUN"));
        Serial.flush();
      }
    }
  }

  delay(20);
}
