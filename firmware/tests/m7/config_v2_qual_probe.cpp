// TEST-ONLY ConfigStore v2 physical qualification probe.
//
// Purpose:
// - exercise the real ConfigStore + NrfConfigFlash + nRF52840 RNG baseline path
//   with SoftDevice disabled;
// - inspect both physical ConfigStore pages using the production v2 classifier;
// - allow an explicit, bounded CLEAN of ONLY 0x0E9000..0x0EAFFF;
// - verify cold-boot token/incarnation persistence without requiring SWD tools.
//
// This is NOT production firmware and must never be left on a deployed unit.
#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <nrf_sdm.h>

#include <string.h>

#include "config_format.h"
#include "config_incarnation_source.h"
#include "config_store.h"
#include "flash_backend.h"
#include "storage_config.h"

using namespace orun_tlp;
using namespace orun_tlp::storage_config;

namespace {

NrfConfigFlash config_flash;
NrfConfigIncarnationSource incarnation_source;
ConfigStore config_store(config_flash, &incarnation_source);

bool terminal = false;
uint32_t last_terminal_report_ms = 0;
char command_buffer[20]{};
uint8_t command_length = 0;
char final_report[128] = "CONFIG V2 QUAL NOT TERMINAL";

void setTerminalReport(const char* text) {
  strncpy(final_report, text, sizeof(final_report) - 1);
  final_report[sizeof(final_report) - 1] = '\0';
  Serial.println(final_report);
  Serial.flush();
  last_terminal_report_ms = millis();
  terminal = true;
}

const char* evidenceName(config_format::PageEvidence evidence) {
  using E = config_format::PageEvidence;
  switch (evidence) {
    case E::kErased: return "ERASED";
    case E::kLegacyV1Committed: return "LEGACY_V1_COMMITTED";
    case E::kLegacyV1UncommittedOrTorn: return "LEGACY_V1_TORN";
    case E::kLegacyV1CommittedCorrupt: return "LEGACY_V1_CORRUPT";
    case E::kV2Staged: return "V2_STAGED";
    case E::kV2UncommittedOrTorn: return "V2_TORN";
    case E::kV2PartialCommit: return "V2_PARTIAL_COMMIT";
    case E::kV2Committed: return "V2_COMMITTED";
    case E::kV2CommittedRetired: return "V2_COMMITTED_RETIRED";
    case E::kV2CommittedCorrupt: return "V2_COMMITTED_CORRUPT";
    case E::kSupportedCorrupt: return "SUPPORTED_CORRUPT";
    case E::kUnsupportedNewer: return "UNSUPPORTED_NEWER";
  }
  return "UNKNOWN";
}

const char* tokenStateName(ConfigTokenState state) {
  switch (state) {
    case ConfigTokenState::kUnavailable: return "UNAVAILABLE";
    case ConfigTokenState::kValid: return "VALID";
    case ConfigTokenState::kUncertain: return "UNCERTAIN";
  }
  return "UNKNOWN";
}

bool softDeviceDisabled() {
  uint8_t enabled = 1;
  return sd_softdevice_is_enabled(&enabled) == NRF_SUCCESS && enabled == 0;
}

bool regionErased() {
  constexpr uint32_t kRegionSize =
      kFutureConfigRegionEnd - kFutureConfigRegionStart;
  uint8_t bytes[64];

  for (uint32_t offset = 0; offset < kRegionSize; offset += sizeof(bytes)) {
    const size_t remaining = kRegionSize - offset;
    const size_t chunk = remaining < sizeof(bytes) ? remaining : sizeof(bytes);
    if (!config_flash.read(offset, bytes, chunk)) return false;
    for (size_t i = 0; i < chunk; ++i) {
      if (bytes[i] != 0xFFU) return false;
    }
  }
  return true;
}

bool pageTailErased(uint32_t page, bool& erased) {
  erased = true;
  uint8_t bytes[64];
  for (uint32_t offset = config_format::kV2PagePrefixSize;
       offset < kPageSize; offset += sizeof(bytes)) {
    const size_t remaining = kPageSize - offset;
    const size_t chunk = remaining < sizeof(bytes) ? remaining : sizeof(bytes);
    if (!config_flash.read(page * kPageSize + offset, bytes, chunk)) return false;
    for (size_t i = 0; i < chunk; ++i) {
      if (bytes[i] != 0xFFU) {
        erased = false;
        return true;
      }
    }
  }
  return true;
}

void printU64Hex(uint64_t value) {
  Serial.printf("%08lX%08lX",
                static_cast<unsigned long>(static_cast<uint32_t>(value >> 32)),
                static_cast<unsigned long>(static_cast<uint32_t>(value)));
}

void printPageStatus(uint32_t page) {
  uint8_t prefix[config_format::kV2PagePrefixSize]{};
  config_format::PageInspection inspection;
  if (!config_flash.read(page * kPageSize, prefix, sizeof(prefix)) ||
      !config_format::inspectPagePrefix(prefix, sizeof(prefix), inspection)) {
    Serial.printf("CONFIG V2 PAGE %c read=FAIL\n", page == 0 ? 'A' : 'B');
    return;
  }

  bool tail_erased = false;
  const bool tail_known =
      inspection.evidence != config_format::PageEvidence::kUnsupportedNewer &&
      pageTailErased(page, tail_erased);

  Serial.printf("CONFIG V2 PAGE %c evidence=%s decoded=%s ",
                page == 0 ? 'A' : 'B',
                evidenceName(inspection.evidence),
                inspection.has_decoded_record ? "yes" : "no");
  if (tail_known)
    Serial.printf("tail_erased=%s ", tail_erased ? "yes" : "no");
  else
    Serial.print(F("tail_erased=not-checked "));

  if (inspection.has_decoded_record) {
    Serial.print(F("generation=0x"));
    printU64Hex(inspection.generation);
    Serial.print(F(" incarnation=0x"));
    printU64Hex(inspection.token.incarnation);
    Serial.printf(" revision=%lu tracking_interval_seconds=%lu battery_capacity_mah=%lu",
                  static_cast<unsigned long>(inspection.token.revision),
                  static_cast<unsigned long>(
                      inspection.config.tracking_interval_seconds),
                  static_cast<unsigned long>(
                      inspection.config.battery_capacity_mah));
  }
  Serial.println();
}

void printStatus() {
  Serial.printf(
      "CONFIG V2 STORE ready=%s maintenance=%s token_state=%s committed_override=%s "
      "tracking_interval_seconds=%lu battery_capacity_mah=%lu "
      "baseline_commits=%lu baseline_failures=%lu\n",
      config_store.ready() ? "yes" : "no",
      config_store.maintenanceResetRequired() ? "yes" : "no",
      tokenStateName(config_store.tokenState()),
      config_store.hasCommittedRecord() ? "yes" : "no",
      static_cast<unsigned long>(config_store.config().tracking_interval_seconds),
      static_cast<unsigned long>(config_store.config().battery_capacity_mah),
      static_cast<unsigned long>(config_store.diagnostics().baseline_commits),
      static_cast<unsigned long>(config_store.diagnostics().baseline_failures));

  config_format::StateToken token;
  if (config_store.stateToken(token)) {
    Serial.print(F("CONFIG V2 TOKEN incarnation=0x"));
    printU64Hex(token.incarnation);
    Serial.printf(" revision=%lu\n",
                  static_cast<unsigned long>(token.revision));
  } else {
    Serial.println(F("CONFIG V2 TOKEN unavailable"));
  }

  printPageStatus(0);
  printPageStatus(1);
  Serial.flush();
}

void cleanConfigPartition() {
  Serial.println(F("CONFIG V2 CLEAN accepted scope=0x0E9000..0x0EAFFF"));
  Serial.flush();

  if (!softDeviceDisabled()) {
    setTerminalReport("CONFIG V2 CLEAN FAIL softdevice_enabled; POWER-CYCLE BEFORE RETRY");
    return;
  }
  if (!config_flash.begin()) {
    setTerminalReport("CONFIG V2 CLEAN FAIL config_flash_begin; POWER-CYCLE BEFORE RETRY");
    return;
  }

  for (uint32_t page = 0; page < kFutureConfigRegionPages; ++page) {
    if (config_flash.erasePage(page) != FlashOpResult::kDone) {
      snprintf(final_report, sizeof(final_report),
               "CONFIG V2 CLEAN FAIL erase page=%lu; POWER-CYCLE BEFORE RETRY",
               static_cast<unsigned long>(page));
      setTerminalReport(final_report);
      return;
    }
  }

  if (!regionErased()) {
    setTerminalReport("CONFIG V2 CLEAN FAIL verify; POWER-CYCLE BEFORE RETRY");
    return;
  }

  setTerminalReport("CONFIG V2 CLEAN PASS pages=2 all_ff=yes; POWER-CYCLE NOW");
}

void handleCommand() {
  command_buffer[command_length] = '\0';

  if (strcmp(command_buffer, "STATUS") == 0) {
    printStatus();
  } else if (strcmp(command_buffer, "CLEAN") == 0) {
    cleanConfigPartition();
  } else if (command_length != 0) {
    Serial.println(F("CONFIG V2 QUAL command rejected; use STATUS or CLEAN"));
    Serial.flush();
  }

  command_length = 0;
}

}  // namespace

void setup() {
  Serial.begin(115200);
  const uint32_t wait_started = millis();
  while (!Serial && (millis() - wait_started) < 15000U) delay(10);

  Serial.println(F("CONFIG V2 PHYSICAL QUAL BOOT"));
  Serial.println(F("TEST-ONLY: destructive command owns ONLY ConfigStore 0x0E9000..0x0EAFFF"));
  Serial.flush();

  if (!softDeviceDisabled()) {
    Serial.println(F("CONFIG V2 QUAL FAIL softdevice_enabled"));
    Serial.flush();
    return;
  }

  const bool begin_ok = config_store.begin();
  Serial.printf("CONFIG V2 BEGIN %s\n", begin_ok ? "PASS" : "FAIL");
  printStatus();
  Serial.println(F("CONFIG V2 QUAL READY commands=STATUS,CLEAN"));
  Serial.flush();
}

void loop() {
  if (terminal) {
    if (Serial && (millis() - last_terminal_report_ms) >= 3000U) {
      Serial.println(final_report);
      Serial.flush();
      last_terminal_report_ms = millis();
    }
    delay(20);
    return;
  }

  while (Serial.available() > 0) {
    const char ch = static_cast<char>(Serial.read());
    if (ch == '\r') continue;

    if (ch == '\n') {
      handleCommand();
      break;
    }

    if (command_length + 1U < sizeof(command_buffer)) {
      command_buffer[command_length++] = ch;
    } else {
      command_length = 0;
      Serial.println(F("CONFIG V2 QUAL command too long; use STATUS or CLEAN"));
      Serial.flush();
    }
  }

  delay(20);
}
