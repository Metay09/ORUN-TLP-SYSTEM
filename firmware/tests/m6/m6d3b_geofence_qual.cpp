// M6D3B TEST-ONLY destructive GeofenceStore qualification.
//
// PRECONDITION: the separate read-only preflight image reported BOTH pages
// all_ff=yes. This image then exercises the real GeofenceStore +
// NrfGeofenceFlash + nRF52840 RNG path with SoftDevice disabled.
// Never deploy this image.
#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <nrf_sdm.h>

#include <stdio.h>
#include <string.h>

#include "flash_backend.h"
#include "geofence_format.h"
#include "geofence_incarnation_source.h"
#include "geofence_store.h"
#include "storage_config.h"

using namespace orun_tlp;
using namespace orun_tlp::storage_config;

namespace {

NrfGeofenceFlash geofence_flash;
NrfGeofenceIncarnationSource incarnation_source;
GeofenceStore geofence_store(geofence_flash, &incarnation_source);

char command_buffer[20]{};
uint8_t command_length = 0;
bool terminal = false;
char terminal_report[128] = "M6D3B QUAL NOT TERMINAL";
uint32_t last_terminal_report_ms = 0;

const GeoPointE7 kFixtureVertices[] = {
    GeoPointE7(378000000, 280000000),
    GeoPointE7(378010000, 280000000),
    GeoPointE7(378000000, 280010000),
};
const GeofencePolygonView kFixturePolygon(kFixtureVertices, 3);

const char* resourceStateName(GeofenceResourceState state) {
  switch (state) {
    case GeofenceResourceState::kUnavailable: return "UNAVAILABLE";
    case GeofenceResourceState::kClear: return "CLEAR";
    case GeofenceResourceState::kConfigured: return "CONFIGURED";
  }
  return "UNKNOWN";
}

const char* tokenStateName(GeofenceTokenState state) {
  switch (state) {
    case GeofenceTokenState::kUnavailable: return "UNAVAILABLE";
    case GeofenceTokenState::kValid: return "VALID";
    case GeofenceTokenState::kUncertain: return "UNCERTAIN";
  }
  return "UNKNOWN";
}

const char* evidenceName(geofence_format::PageEvidence evidence) {
  using E = geofence_format::PageEvidence;
  switch (evidence) {
    case E::kErased: return "ERASED";
    case E::kStaged: return "STAGED";
    case E::kUncommittedOrTorn: return "UNCOMMITTED_OR_TORN";
    case E::kPartialCommit: return "PARTIAL_COMMIT";
    case E::kCommittedClear: return "COMMITTED_CLEAR";
    case E::kCommittedConfigured: return "COMMITTED_CONFIGURED";
    case E::kCommittedCorrupt: return "COMMITTED_CORRUPT";
    case E::kUnsupportedNewer: return "UNSUPPORTED_NEWER";
    case E::kSupportedCorrupt: return "SUPPORTED_CORRUPT";
  }
  return "UNKNOWN";
}

void printU64Hex(uint64_t value) {
  Serial.printf("%08lX%08lX",
      static_cast<unsigned long>(static_cast<uint32_t>(value >> 32)),
      static_cast<unsigned long>(static_cast<uint32_t>(value)));
}

bool softDeviceDisabled() {
  uint8_t enabled = 1;
  return sd_softdevice_is_enabled(&enabled) == NRF_SUCCESS && enabled == 0;
}

bool regionErased() {
  uint8_t bytes[64];
  constexpr uint32_t kRegionSize =
      kGeofenceRegionEnd - kGeofenceRegionStart;
  for (uint32_t offset = 0; offset < kRegionSize; offset += sizeof(bytes)) {
    const size_t remaining = kRegionSize - offset;
    const size_t chunk =
        remaining < sizeof(bytes) ? remaining : sizeof(bytes);
    if (!geofence_flash.read(offset, bytes, chunk)) return false;
    for (size_t i = 0; i < chunk; ++i)
      if (bytes[i] != 0xFFU) return false;
  }
  return true;
}

bool tailErased(uint32_t page) {
  uint8_t bytes[64];
  for (uint32_t offset = geofence_format::kRecordSize;
       offset < kPageSize; offset += sizeof(bytes)) {
    const size_t remaining = kPageSize - offset;
    const size_t chunk =
        remaining < sizeof(bytes) ? remaining : sizeof(bytes);
    if (!geofence_flash.read(page * kPageSize + offset, bytes, chunk))
      return false;
    for (size_t i = 0; i < chunk; ++i)
      if (bytes[i] != 0xFFU) return false;
  }
  return true;
}

void printPage(uint32_t page) {
  uint8_t bytes[geofence_format::kRecordSize];
  geofence_format::PageInspection inspection;
  if (!geofence_flash.read(page * kPageSize, bytes, sizeof(bytes)) ||
      !geofence_format::inspectPage(bytes, sizeof(bytes), inspection)) {
    Serial.printf("M6D3B QUAL PAGE %c read=FAIL\n", page == 0 ? 'A' : 'B');
    return;
  }

  Serial.printf("M6D3B QUAL PAGE %c evidence=%s decoded=%s tail_ff=%s",
                page == 0 ? 'A' : 'B',
                evidenceName(inspection.evidence),
                inspection.has_decoded_record ? "yes" : "no",
                tailErased(page) ? "yes" : "no");
  if (inspection.has_decoded_record) {
    Serial.print(F(" generation=0x"));
    printU64Hex(inspection.record.generation);
    Serial.print(F(" incarnation=0x"));
    printU64Hex(inspection.record.token.incarnation);
    Serial.printf(" revision=%lu state=%s areas=%u vertices=%u",
                  static_cast<unsigned long>(
                      inspection.record.token.revision),
                  inspection.record.snapshot.state ==
                          geofence_format::ResourceState::kClear
                      ? "CLEAR" : "CONFIGURED",
                  static_cast<unsigned>(
                      inspection.record.snapshot.area_count),
                  static_cast<unsigned>(
                      inspection.record.snapshot.total_vertex_count));
  }
  Serial.println();
}

void printStatus() {
  Serial.printf(
      "M6D3B QUAL STORE ready=%s busy=%s maintenance=%s resource=%s "
      "token_state=%s mutations=%lu failures=%lu reconciliations=%lu\n",
      geofence_store.ready() ? "yes" : "no",
      geofence_store.busy() ? "yes" : "no",
      geofence_store.maintenanceResetRequired() ? "yes" : "no",
      resourceStateName(geofence_store.resourceState()),
      tokenStateName(geofence_store.tokenState()),
      static_cast<unsigned long>(geofence_store.diagnostics().mutations),
      static_cast<unsigned long>(
          geofence_store.diagnostics().mutation_failures),
      static_cast<unsigned long>(
          geofence_store.diagnostics().recovery_reconciliations));

  geofence_format::StateToken token;
  if (geofence_store.stateToken(token)) {
    Serial.print(F("M6D3B QUAL TOKEN incarnation=0x"));
    printU64Hex(token.incarnation);
    Serial.printf(" revision=%lu\n",
                  static_cast<unsigned long>(token.revision));
  } else {
    Serial.println(F("M6D3B QUAL TOKEN unavailable"));
  }

  geofence_format::Snapshot snapshot;
  if (geofence_store.currentSnapshot(snapshot)) {
    Serial.printf("M6D3B QUAL SNAPSHOT state=%s areas=%u vertices=%u\n",
                  snapshot.state == geofence_format::ResourceState::kClear
                      ? "CLEAR" : "CONFIGURED",
                  static_cast<unsigned>(snapshot.area_count),
                  static_cast<unsigned>(snapshot.total_vertex_count));
  }

  printPage(0);
  printPage(1);
  Serial.flush();
}

void settleAndReport(const char* operation) {
  for (unsigned pass = 0; pass < 1000 && geofence_store.busy(); ++pass)
    geofence_store.poll();

  if (geofence_store.busy()) {
    Serial.printf("M6D3B QUAL %s FAIL busy_timeout\n", operation);
    return;
  }

  geofence_store.poll();
  bool success = false;
  if (!geofence_store.takeMutationResult(success)) {
    Serial.printf("M6D3B QUAL %s NO_RESULT\n", operation);
  } else {
    Serial.printf("M6D3B QUAL %s result=%s\n",
                  operation, success ? "CONFIRMED" : "UNCONFIRMED");
  }
  printStatus();
}

void setTerminal(const char* report) {
  strncpy(terminal_report, report, sizeof(terminal_report) - 1);
  terminal_report[sizeof(terminal_report) - 1] = '\0';
  Serial.println(terminal_report);
  Serial.flush();
  terminal = true;
  last_terminal_report_ms = millis();
}

void cleanPartition() {
  Serial.println(F(
      "M6D3B QUAL CLEAN accepted destructive scope=0x0E5000..0x0E6FFF"));
  Serial.flush();

  if (!softDeviceDisabled()) {
    setTerminal("M6D3B QUAL CLEAN FAIL softdevice_enabled");
    return;
  }

  for (uint32_t page = 0; page < kGeofenceRegionPages; ++page) {
    if (geofence_flash.erasePage(page) != FlashOpResult::kDone) {
      setTerminal("M6D3B QUAL CLEAN FAIL erase; POWER-CYCLE BEFORE RETRY");
      return;
    }
  }
  if (!regionErased()) {
    setTerminal("M6D3B QUAL CLEAN FAIL verify; POWER-CYCLE BEFORE RETRY");
    return;
  }

  setTerminal("M6D3B QUAL CLEAN PASS all_ff=yes; POWER-CYCLE NOW");
}

void handleCommand() {
  command_buffer[command_length] = '\0';

  if (strcmp(command_buffer, "STATUS") == 0) {
    printStatus();
  } else if (strcmp(command_buffer, "REPLACE") == 0) {
    if (!geofence_store.requestReplace(
            GeofenceAreaSetView(&kFixturePolygon, 1))) {
      Serial.println(F("M6D3B QUAL REPLACE REJECTED"));
    } else if (!geofence_store.busy()) {
      Serial.println(F("M6D3B QUAL REPLACE ALREADY_SATISFIED"));
      printStatus();
    } else {
      settleAndReport("REPLACE");
    }
  } else if (strcmp(command_buffer, "CLEAR") == 0) {
    if (!geofence_store.requestClear()) {
      Serial.println(F("M6D3B QUAL CLEAR REJECTED"));
    } else if (!geofence_store.busy()) {
      Serial.println(F("M6D3B QUAL CLEAR ALREADY_SATISFIED"));
      printStatus();
    } else {
      settleAndReport("CLEAR");
    }
  } else if (strcmp(command_buffer, "CLEAN") == 0) {
    cleanPartition();
  } else if (command_length != 0) {
    Serial.println(F(
        "M6D3B QUAL command rejected; use STATUS,REPLACE,CLEAR,CLEAN"));
  }

  command_length = 0;
  Serial.flush();
}

}  // namespace

void setup() {
  Serial.begin(115200);
  const uint32_t started = millis();
  while (!Serial && (millis() - started) < 15000U) delay(10);

  Serial.println(F("M6D3B GEOFENCE QUAL BOOT"));
  Serial.println(F(
      "TEST-ONLY DESTRUCTIVE: use ONLY after read-only PREFLIGHT all_ff=yes"));
  Serial.println(F("scope=0x0E5000..0x0E6FFF"));

  if (!softDeviceDisabled()) {
    Serial.println(F("M6D3B QUAL FAIL softdevice_enabled"));
    return;
  }

  const bool begin_ok = geofence_store.begin();
  Serial.printf("M6D3B QUAL BEGIN %s\n", begin_ok ? "PASS" : "FAIL");
  printStatus();
  Serial.println(F("M6D3B QUAL READY commands=STATUS,REPLACE,CLEAR,CLEAN"));
  Serial.flush();
}

void loop() {
  if (terminal) {
    if (Serial && (millis() - last_terminal_report_ms) >= 3000U) {
      Serial.println(terminal_report);
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
      Serial.println(F("M6D3B QUAL command too long"));
    }
  }

  geofence_store.poll();
  delay(20);
}
