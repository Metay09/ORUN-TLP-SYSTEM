// TEST-ONLY ConfigStore v2 fresh-baseline power-cut probe.
//
// On a BLANK ConfigStore partition this uses the real ConfigStore,
// NrfConfigIncarnationSource and NrfConfigFlash. The wrapper delegates the
// real body+CRC program, lets ConfigStore perform its real stage readback, then
// deliberately blocks the exact 4-byte commit program before touching flash.
// The operator removes physical power while the probe repeats CUT POWER NOW.
//
// After reboot with the same image, the real ConfigStore recovery path observes
// the staged page. No write should occur on that recovery boot.
//
// Never deploy this image.
#include <Arduino.h>
#include <Adafruit_TinyUSB.h>

#include "config_format.h"
#include "config_incarnation_source.h"
#include "config_store.h"
#include "flash_backend.h"
#include "storage_config.h"

using namespace orun_tlp;
using namespace orun_tlp::storage_config;

namespace {

class BaselineCommitCutBackend final : public FlashBackend {
 public:
  bool begin() override { return physical_.begin(); }

  bool read(uint32_t offset, void* data, size_t size) const override {
    return physical_.read(offset, data, size);
  }

  FlashOpResult program(uint32_t offset, const void* data, size_t size) override {
    if (offset == config_format::kV2CommitOffset && size == 4U) {
      // Reaching this call proves ConfigStore already:
      // 1) programmed the real 44-byte body+CRC through physical_, and
      // 2) read it back successfully in writeFreshBaseline().
      // Do NOT delegate the commit. Hold here until real power is removed.
      while (true) {
        Serial.println(
            F("CONFIG V2 BASELINE CUT READY stage=after-body-readback before-commit; CUT POWER NOW"));
        Serial.flush();
        delay(1000);
      }
    }
    return physical_.program(offset, data, size);
  }

  FlashOpResult erasePage(uint32_t page) override {
    return physical_.erasePage(page);
  }

 private:
  NrfConfigFlash physical_;
};

BaselineCommitCutBackend cut_backend;
NrfConfigIncarnationSource incarnation_source;
ConfigStore config_store(cut_backend, &incarnation_source);

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

bool tailErased(uint32_t page, bool& erased) {
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
    Serial.printf("CONFIG V2 CUT PAGE %c read=FAIL\n", page == 0 ? 'A' : 'B');
    return;
  }

  bool tail_erased = false;
  const bool tail_known =
      inspection.evidence != config_format::PageEvidence::kUnsupportedNewer &&
      tailErased(page, tail_erased);

  Serial.printf("CONFIG V2 CUT PAGE %c evidence=%s decoded=%s ",
                page == 0 ? 'A' : 'B', evidenceName(inspection.evidence),
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

void printRecoveredStatus(bool begin_ok) {
  Serial.printf(
      "CONFIG V2 CUT RECOVERY begin=%s ready=%s maintenance=%s token_state=%s "
      "committed_override=%s baseline_commits=%lu baseline_failures=%lu "
      "tracking_interval_seconds=%lu battery_capacity_mah=%lu\n",
      begin_ok ? "PASS" : "FAIL",
      config_store.ready() ? "yes" : "no",
      config_store.maintenanceResetRequired() ? "yes" : "no",
      tokenStateName(config_store.tokenState()),
      config_store.hasCommittedRecord() ? "yes" : "no",
      static_cast<unsigned long>(config_store.diagnostics().baseline_commits),
      static_cast<unsigned long>(config_store.diagnostics().baseline_failures),
      static_cast<unsigned long>(config_store.config().tracking_interval_seconds),
      static_cast<unsigned long>(config_store.config().battery_capacity_mah));

  config_format::StateToken token;
  if (config_store.stateToken(token)) {
    Serial.print(F("CONFIG V2 CUT AUTHORITATIVE TOKEN incarnation=0x"));
    printU64Hex(token.incarnation);
    Serial.printf(" revision=%lu\n",
                  static_cast<unsigned long>(token.revision));
  } else {
    Serial.println(F("CONFIG V2 CUT AUTHORITATIVE TOKEN unavailable"));
  }

  printPage(0);
  printPage(1);
  Serial.flush();
}

}  // namespace

void setup() {
  Serial.begin(115200);
  const uint32_t started = millis();
  while (!Serial && (millis() - started) < 15000U) delay(10);

  Serial.println(F("CONFIG V2 BASELINE POWER-CUT PROBE BOOT"));
  Serial.println(F("TEST-ONLY: requires BLANK ConfigStore; commit write is intentionally withheld"));
  Serial.flush();

  // Blank partition: begin() will block inside the backend immediately before
  // the baseline commit and repeatedly print CUT POWER NOW.
  //
  // Reboot after the real cut: the page is V2_STAGED, so begin() returns
  // through the production read-only recovery path and execution reaches the
  // status report below.
  const bool begin_ok = config_store.begin();
  printRecoveredStatus(begin_ok);

  Serial.println(
      F("CONFIG V2 BASELINE CUT RECOVERY COMPLETE; DO NOT TREAT STAGED TOKEN AS AUTHORITATIVE"));
  Serial.flush();
}

void loop() {
  delay(1000);
}
