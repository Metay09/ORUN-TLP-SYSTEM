// TEST-ONLY ConfigStore v2 normal-save power-cut boundary probe.
//
// Requires an existing valid v2 ConfigStore baseline. On explicit serial
// command it submits a real ConfigStore::requestSave() through the real
// NrfConfigFlash backend and blocks at one deterministic persistence boundary:
//
//   CUT_ERASE  - inactive page erase completed + read-verified, body not written
//   CUT_BODY   - body+CRC written and ConfigStore stage-readback completed,
//                commit word not written
//   CUT_COMMIT - commit word physically written + backend read-verified,
//                ConfigStore final-record verify/publication not yet run
//
// The operator removes REAL power only after the repeated CUT POWER NOW line.
// After reboot the cut mode is disarmed; production ConfigStore recovery runs
// read-only and this probe reports the resulting page/token lineage.
//
// This is NOT production firmware and must never be left on a deployed unit.
#include <Arduino.h>
#include <Adafruit_TinyUSB.h>

#include <stdio.h>
#include <string.h>

#include "config_format.h"
#include "config_incarnation_source.h"
#include "config_store.h"
#include "flash_backend.h"
#include "storage_config.h"

using namespace orun_tlp;
using namespace orun_tlp::storage_config;

namespace {

enum class CutMode : uint8_t {
  kNone,
  kAfterErase,
  kAfterBodyReadback,
  kAfterCommit,
};

class NormalSaveCutBackend final : public FlashBackend {
 public:
  bool begin() override { return physical_.begin(); }

  bool read(uint32_t offset, void* data, size_t size) const override {
    return physical_.read(offset, data, size);
  }

  FlashOpResult erasePage(uint32_t page) override {
    const FlashOpResult result = physical_.erasePage(page);
    if (result == FlashOpResult::kDone && armed_ &&
        mode_ == CutMode::kAfterErase) {
      cutForever(
          "CONFIG V2 NORMAL CUT READY stage=after-erase-readback before-body; CUT POWER NOW");
    }
    return result;
  }

  FlashOpResult program(uint32_t offset, const void* data, size_t size) override {
    const bool commit =
        size == 4U &&
        (offset % kPageSize) == config_format::kV2CommitOffset;

    if (armed_ && commit && mode_ == CutMode::kAfterBodyReadback) {
      // ConfigStore can only reach the commit call after the 44-byte body+CRC
      // program returned kDone AND its own body readback/memcmp succeeded.
      // Do not touch the commit word.
      cutForever(
          "CONFIG V2 NORMAL CUT READY stage=after-body-readback before-commit; CUT POWER NOW");
    }

    const FlashOpResult result = physical_.program(offset, data, size);

    if (result == FlashOpResult::kDone && armed_ && commit &&
        mode_ == CutMode::kAfterCommit) {
      // NrfConfigFlash::program() returned only after physical program +
      // readback verification. ConfigStore has not yet run its final full
      // record readback nor finishSave(), because this call has not returned.
      cutForever(
          "CONFIG V2 NORMAL CUT READY stage=after-commit-readback before-final-verify; CUT POWER NOW");
    }

    return result;
  }

  void arm(CutMode mode) {
    mode_ = mode;
    armed_ = mode != CutMode::kNone;
  }

 private:
  [[noreturn]] void cutForever(const char* line) {
    while (true) {
      Serial.println(line);
      Serial.flush();
      delay(1000);
    }
  }

  NrfConfigFlash physical_;
  CutMode mode_ = CutMode::kNone;
  bool armed_ = false;
};

NormalSaveCutBackend cut_backend;
NrfConfigIncarnationSource incarnation_source;
ConfigStore config_store(cut_backend, &incarnation_source);

char command_buffer[24]{};
uint8_t command_length = 0;

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

void printU64Hex(uint64_t value) {
  Serial.printf("%08lX%08lX",
                static_cast<unsigned long>(static_cast<uint32_t>(value >> 32)),
                static_cast<unsigned long>(static_cast<uint32_t>(value)));
}

bool pageTailErased(uint32_t page, bool& erased) {
  erased = true;
  uint8_t bytes[64];
  for (uint32_t offset = config_format::kV2PagePrefixSize;
       offset < kPageSize; offset += sizeof(bytes)) {
    const size_t remaining = kPageSize - offset;
    const size_t chunk = remaining < sizeof(bytes) ? remaining : sizeof(bytes);
    if (!cut_backend.read(page * kPageSize + offset, bytes, chunk)) return false;
    for (size_t i = 0; i < chunk; ++i) {
      if (bytes[i] != 0xFFU) {
        erased = false;
        return true;
      }
    }
  }
  return true;
}

void printPage(uint32_t page) {
  uint8_t prefix[config_format::kV2PagePrefixSize]{};
  config_format::PageInspection inspection;
  if (!cut_backend.read(page * kPageSize, prefix, sizeof(prefix)) ||
      !config_format::inspectPagePrefix(prefix, sizeof(prefix), inspection)) {
    Serial.printf("CONFIG V2 NORMAL PAGE %c read=FAIL\n", page == 0 ? 'A' : 'B');
    return;
  }

  bool tail_erased = false;
  const bool tail_known =
      inspection.evidence != config_format::PageEvidence::kUnsupportedNewer &&
      pageTailErased(page, tail_erased);

  Serial.printf("CONFIG V2 NORMAL PAGE %c evidence=%s decoded=%s ",
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
      "CONFIG V2 NORMAL STORE ready=%s maintenance=%s token_state=%s "
      "committed_override=%s busy=%s tracking_interval_seconds=%lu "
      "battery_capacity_mah=%lu saves=%lu save_failures=%lu "
      "baseline_commits=%lu baseline_failures=%lu\n",
      config_store.ready() ? "yes" : "no",
      config_store.maintenanceResetRequired() ? "yes" : "no",
      tokenStateName(config_store.tokenState()),
      config_store.hasCommittedRecord() ? "yes" : "no",
      config_store.busy() ? "yes" : "no",
      static_cast<unsigned long>(config_store.config().tracking_interval_seconds),
      static_cast<unsigned long>(config_store.config().battery_capacity_mah),
      static_cast<unsigned long>(config_store.diagnostics().saves),
      static_cast<unsigned long>(config_store.diagnostics().save_failures),
      static_cast<unsigned long>(config_store.diagnostics().baseline_commits),
      static_cast<unsigned long>(config_store.diagnostics().baseline_failures));

  config_format::StateToken token;
  if (config_store.stateToken(token)) {
    Serial.print(F("CONFIG V2 NORMAL TOKEN incarnation=0x"));
    printU64Hex(token.incarnation);
    Serial.printf(" revision=%lu\n",
                  static_cast<unsigned long>(token.revision));
  } else {
    Serial.println(F("CONFIG V2 NORMAL TOKEN unavailable"));
  }

  printPage(0);
  printPage(1);
  Serial.flush();
}

void runCut(CutMode mode, const char* name) {
  if (!config_store.ready() ||
      config_store.maintenanceResetRequired() ||
      config_store.tokenState() != ConfigTokenState::kValid ||
      config_store.busy()) {
    Serial.printf("CONFIG V2 NORMAL %s rejected store_not_mutable\n", name);
    printStatus();
    return;
  }

  config_format::Config candidate = config_store.config();
  candidate.battery_capacity_mah =
      candidate.battery_capacity_mah == 0U ? 1U : 0U;

  Serial.printf(
      "CONFIG V2 NORMAL %s accepted candidate tracking_interval_seconds=%lu "
      "battery_capacity_mah=%lu\n",
      name,
      static_cast<unsigned long>(candidate.tracking_interval_seconds),
      static_cast<unsigned long>(candidate.battery_capacity_mah));
  Serial.flush();

  cut_backend.arm(mode);
  if (!config_store.requestSave(candidate)) {
    cut_backend.arm(CutMode::kNone);
    Serial.printf("CONFIG V2 NORMAL %s FAIL requestSave\n", name);
    Serial.flush();
    return;
  }

  // Synchronous NrfConfigFlash means a small bounded number of poll passes
  // reaches the selected wrapper cut point. If it ever completes instead of
  // blocking, fail visibly rather than silently continuing.
  for (uint32_t pass = 0; pass < 16U; ++pass) {
    config_store.poll();

    bool success = false;
    if (config_store.takeSaveResult(success)) {
      cut_backend.arm(CutMode::kNone);
      Serial.printf(
          "CONFIG V2 NORMAL %s FAIL unexpected_save_result success=%s\n",
          name, success ? "yes" : "no");
      printStatus();
      return;
    }
  }

  cut_backend.arm(CutMode::kNone);
  Serial.printf("CONFIG V2 NORMAL %s FAIL cut_point_not_reached\n", name);
  printStatus();
}

void handleCommand() {
  command_buffer[command_length] = '\0';

  if (strcmp(command_buffer, "STATUS") == 0) {
    printStatus();
  } else if (strcmp(command_buffer, "CUT_ERASE") == 0) {
    runCut(CutMode::kAfterErase, "CUT_ERASE");
  } else if (strcmp(command_buffer, "CUT_BODY") == 0) {
    runCut(CutMode::kAfterBodyReadback, "CUT_BODY");
  } else if (strcmp(command_buffer, "CUT_COMMIT") == 0) {
    runCut(CutMode::kAfterCommit, "CUT_COMMIT");
  } else if (command_length != 0) {
    Serial.println(
        F("CONFIG V2 NORMAL command rejected; use STATUS,CUT_ERASE,CUT_BODY,CUT_COMMIT"));
    Serial.flush();
  }

  command_length = 0;
}

}  // namespace

void setup() {
  Serial.begin(115200);
  const uint32_t started = millis();
  while (!Serial && (millis() - started) < 15000U) delay(10);

  Serial.println(F("CONFIG V2 NORMAL-SAVE POWER-CUT PROBE BOOT"));
  Serial.println(
      F("TEST-ONLY: explicit commands mutate ONLY ConfigStore through real requestSave/poll"));
  Serial.flush();

  const bool begin_ok = config_store.begin();
  Serial.printf("CONFIG V2 NORMAL BEGIN %s\n", begin_ok ? "PASS" : "FAIL");
  printStatus();
  Serial.println(
      F("CONFIG V2 NORMAL READY commands=STATUS,CUT_ERASE,CUT_BODY,CUT_COMMIT"));
  Serial.flush();
}

void loop() {
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
      Serial.println(
          F("CONFIG V2 NORMAL command too long; use STATUS,CUT_ERASE,CUT_BODY,CUT_COMMIT"));
      Serial.flush();
    }
  }

  delay(20);
}
