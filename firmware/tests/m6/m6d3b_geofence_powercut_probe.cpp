// M6D3B TEST-ONLY normal-mutation electrical power-cut probe.
//
// PRECONDITION:
//   - the same development unit has already passed M6D3B read-only preflight;
//   - GeofenceStore currently has one VALID committed authority.
//
// The probe boots/recoveries read-only. Only the explicit CUT_BODY command
// arms a mutation. It submits one semantic successor through the real
// GeofenceStore + NrfGeofenceFlash path, allows the inactive page erase and
// body+CRC program/readback to complete, then deliberately withholds the exact
// 4-byte commit write.
//
// The operator removes REAL power only after the repeated CUT POWER NOW line.
// On reboot the cut mode is disarmed. Production GeofenceStore recovery then
// sees an exact staged successor (commit word still erased) and must retain the
// previously committed token/snapshot as VALID authority.
//
// Never deploy this image.
#include <Arduino.h>
#include <Adafruit_TinyUSB.h>

#include <stdio.h>
#include <string.h>

#include "flash_backend.h"
#include "geofence_format.h"
#include "geofence_store.h"
#include "storage_config.h"

using namespace orun_tlp;
using namespace orun_tlp::storage_config;

namespace {

class BodyCommitCutBackend final : public FlashBackend {
 public:
  bool begin() override { return physical_.begin(); }

  bool read(uint32_t offset, void* data, size_t size) const override {
    return physical_.read(offset, data, size);
  }

  FlashOpResult erasePage(uint32_t page) override {
    return physical_.erasePage(page);
  }

  FlashOpResult program(uint32_t offset, const void* data, size_t size) override {
    const bool commit =
        size == 4U && (offset % kPageSize) == geofence_format::kCommitOffset;

    if (armed_ && commit) {
      // GeofenceStore reaches this call only after:
      // 1) the inactive page was physically erased + verified,
      // 2) the 560-byte body+CRC was physically programmed + verified, and
      // 3) GeofenceStore performed its own body readback/memcmp.
      // Do not touch the commit word. Real power must be removed here.
      while (true) {
        Serial.println(
            F("M6D3B POWERCUT READY stage=after-body-readback before-commit; CUT POWER NOW"));
        Serial.flush();
        delay(1000);
      }
    }

    return physical_.program(offset, data, size);
  }

  void arm() { armed_ = true; }

 private:
  NrfGeofenceFlash physical_;
  bool armed_ = false;
};

BodyCommitCutBackend cut_backend;
// Intentionally no incarnation source: this probe must never manufacture a
// baseline. A blank/invalid partition therefore remains non-mutable.
GeofenceStore geofence_store(cut_backend, nullptr);

char command_buffer[20]{};
uint8_t command_length = 0;

// Keep record-sized diagnostic workspaces out of the 4-KiB loop task stack.
uint8_t page_bytes[geofence_format::kRecordSize]{};
geofence_format::PageInspection page_inspection;
geofence_format::StateToken status_token;
geofence_format::Snapshot status_snapshot;

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

bool tailErased(uint32_t page) {
  uint8_t bytes[64];
  for (uint32_t offset = geofence_format::kRecordSize;
       offset < kPageSize; offset += sizeof(bytes)) {
    const size_t remaining = kPageSize - offset;
    const size_t chunk = remaining < sizeof(bytes) ? remaining : sizeof(bytes);
    if (!cut_backend.read(page * kPageSize + offset, bytes, chunk)) return false;
    for (size_t i = 0; i < chunk; ++i)
      if (bytes[i] != 0xFFU) return false;
  }
  return true;
}

void printPage(uint32_t page) {
  page_inspection = geofence_format::PageInspection();
  if (!cut_backend.read(page * kPageSize, page_bytes, sizeof(page_bytes)) ||
      !geofence_format::inspectPage(page_bytes, sizeof(page_bytes),
                                    page_inspection)) {
    Serial.printf("M6D3B POWERCUT PAGE %c read=FAIL\n", page == 0 ? 'A' : 'B');
    return;
  }

  Serial.printf("M6D3B POWERCUT PAGE %c evidence=%s decoded=%s tail_ff=%s",
                page == 0 ? 'A' : 'B',
                evidenceName(page_inspection.evidence),
                page_inspection.has_decoded_record ? "yes" : "no",
                tailErased(page) ? "yes" : "no");

  if (page_inspection.has_decoded_record) {
    Serial.print(F(" generation=0x"));
    printU64Hex(page_inspection.record.generation);
    Serial.print(F(" incarnation=0x"));
    printU64Hex(page_inspection.record.token.incarnation);
    Serial.printf(" revision=%lu state=%s areas=%u vertices=%u",
                  static_cast<unsigned long>(
                      page_inspection.record.token.revision),
                  page_inspection.record.snapshot.state ==
                          geofence_format::ResourceState::kClear
                      ? "CLEAR" : "CONFIGURED",
                  static_cast<unsigned>(
                      page_inspection.record.snapshot.area_count),
                  static_cast<unsigned>(
                      page_inspection.record.snapshot.total_vertex_count));
  }
  Serial.println();
}

void printStatus() {
  Serial.printf(
      "M6D3B POWERCUT STORE ready=%s busy=%s maintenance=%s resource=%s "
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

  if (geofence_store.stateToken(status_token)) {
    Serial.print(F("M6D3B POWERCUT TOKEN incarnation=0x"));
    printU64Hex(status_token.incarnation);
    Serial.printf(" revision=%lu\n",
                  static_cast<unsigned long>(status_token.revision));
  } else {
    Serial.println(F("M6D3B POWERCUT TOKEN unavailable"));
  }

  if (geofence_store.currentSnapshot(status_snapshot)) {
    Serial.printf("M6D3B POWERCUT SNAPSHOT state=%s areas=%u vertices=%u\n",
                  status_snapshot.state ==
                          geofence_format::ResourceState::kClear
                      ? "CLEAR" : "CONFIGURED",
                  static_cast<unsigned>(status_snapshot.area_count),
                  static_cast<unsigned>(status_snapshot.total_vertex_count));
  }

  printPage(0);
  printPage(1);
  Serial.flush();
}

void runBodyCut() {
  if (!geofence_store.ready() || geofence_store.busy() ||
      geofence_store.maintenanceResetRequired() ||
      geofence_store.tokenState() != GeofenceTokenState::kValid) {
    Serial.println(F("M6D3B POWERCUT CUT_BODY rejected store_not_mutable"));
    printStatus();
    return;
  }

  // Always request the opposite semantic state so this command is never an
  // unchanged no-op on a valid store.
  const bool current_is_clear =
      geofence_store.resourceState() == GeofenceResourceState::kClear;

  Serial.printf("M6D3B POWERCUT CUT_BODY accepted candidate=%s\n",
                current_is_clear ? "CONFIGURED" : "CLEAR");
  Serial.flush();

  cut_backend.arm();

  const bool accepted =
      current_is_clear
          ? geofence_store.requestReplace(
                GeofenceAreaSetView(&kFixturePolygon, 1))
          : geofence_store.requestClear();

  if (!accepted || !geofence_store.busy()) {
    Serial.println(F("M6D3B POWERCUT CUT_BODY FAIL request_not_started"));
    printStatus();
    return;
  }

  // Synchronous NrfGeofenceFlash reaches the commit boundary in a bounded
  // number of poll passes. The backend blocks forever at that boundary.
  for (uint32_t pass = 0; pass < 16U; ++pass) {
    geofence_store.poll();

    bool success = false;
    if (geofence_store.takeMutationResult(success)) {
      Serial.printf(
          "M6D3B POWERCUT CUT_BODY FAIL unexpected_result success=%s\n",
          success ? "yes" : "no");
      printStatus();
      return;
    }
  }

  Serial.println(F("M6D3B POWERCUT CUT_BODY FAIL cut_point_not_reached"));
  printStatus();
}

void handleCommand() {
  command_buffer[command_length] = '\0';

  if (strcmp(command_buffer, "STATUS") == 0) {
    printStatus();
  } else if (strcmp(command_buffer, "CUT_BODY") == 0) {
    runBodyCut();
  } else if (command_length != 0) {
    Serial.println(F("M6D3B POWERCUT command rejected; use STATUS,CUT_BODY"));
    Serial.flush();
  }

  command_length = 0;
}

}  // namespace

void setup() {
  Serial.begin(115200);
  const uint32_t started = millis();
  while (!Serial && (millis() - started) < 15000U) delay(10);

  Serial.println(F("M6D3B GEOFENCE POWER-CUT PROBE BOOT"));
  Serial.println(F(
      "TEST-ONLY: boot is read-only; CUT_BODY mutates only the inactive geofence page"));
  Serial.println(F(
      "CUT point: body+CRC physically verified, commit word intentionally withheld"));

  const bool begin_ok = geofence_store.begin();
  Serial.printf("M6D3B POWERCUT BEGIN %s\n", begin_ok ? "PASS" : "FAIL");
  printStatus();
  Serial.println(F("M6D3B POWERCUT READY commands=STATUS,CUT_BODY"));
  Serial.flush();
}

void loop() {
  while (Serial.available() > 0) {
    const char ch = static_cast<char>(Serial.read());
    if (ch == '\r') continue;

    if (ch == '\n') {
      handleCommand();
      continue;
    }

    if (command_length + 1 < sizeof(command_buffer)) {
      command_buffer[command_length++] = ch;
    } else {
      command_length = 0;
      Serial.println(F("M6D3B POWERCUT command too long"));
      Serial.flush();
    }
  }

  delay(10);
}
