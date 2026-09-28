// M6D3B TEST-ONLY read-only GeofenceStore preflight.
//
// This image contains no flash writer/backend. It directly reads and classifies
// 0x0E5000..0x0E7000 so the operator can prove the proposed GeofenceStore
// region is blank before any destructive qualification image is allowed.
#include <Arduino.h>
#include <Adafruit_TinyUSB.h>

#include "geofence_format.h"
#include "journal_format.h"
#include "storage_config.h"

using namespace orun_tlp;
using namespace orun_tlp::storage_config;

namespace {

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

uint32_t pageCrc(uint32_t page) {
  const auto* bytes = reinterpret_cast<const uint8_t*>(
      kGeofenceRegionStart + page * kPageSize);
  return journal_format::crc32(bytes, kPageSize);
}

bool pageAllFf(uint32_t page, uint32_t& first_non_ff_offset,
               uint8_t& first_non_ff_value) {
  const auto* bytes = reinterpret_cast<const uint8_t*>(
      kGeofenceRegionStart + page * kPageSize);
  for (uint32_t i = 0; i < kPageSize; ++i) {
    if (bytes[i] != 0xFFU) {
      first_non_ff_offset = i;
      first_non_ff_value = bytes[i];
      return false;
    }
  }
  first_non_ff_offset = UINT32_MAX;
  first_non_ff_value = 0xFF;
  return true;
}

bool tailAllFf(uint32_t page) {
  const auto* bytes = reinterpret_cast<const uint8_t*>(
      kGeofenceRegionStart + page * kPageSize);
  for (uint32_t i = geofence_format::kRecordSize; i < kPageSize; ++i)
    if (bytes[i] != 0xFFU) return false;
  return true;
}

void printPage(uint32_t page) {
  const auto* bytes = reinterpret_cast<const uint8_t*>(
      kGeofenceRegionStart + page * kPageSize);

  geofence_format::PageInspection inspection;
  const bool inspect_ok =
      geofence_format::inspectPage(bytes, geofence_format::kRecordSize,
                                   inspection);
  uint32_t first_offset = UINT32_MAX;
  uint8_t first_value = 0xFF;
  const bool all_ff = pageAllFf(page, first_offset, first_value);

  Serial.printf(
      "M6D3B PREFLIGHT PAGE %c inspect=%s evidence=%s all_ff=%s "
      "tail_ff=%s crc32=%08lX",
      page == 0 ? 'A' : 'B',
      inspect_ok ? "PASS" : "FAIL",
      inspect_ok ? evidenceName(inspection.evidence) : "NOT_CLASSIFIED",
      all_ff ? "yes" : "no",
      tailAllFf(page) ? "yes" : "no",
      static_cast<unsigned long>(pageCrc(page)));

  if (!all_ff) {
    Serial.printf(" first_non_ff=0x%04lX value=%02X",
                  static_cast<unsigned long>(first_offset),
                  static_cast<unsigned>(first_value));
  }
  Serial.println();
}

}  // namespace

void setup() {
  Serial.begin(115200);
  const uint32_t started = millis();
  while (!Serial && (millis() - started) < 15000U) delay(10);

  Serial.println(F("M6D3B GEOFENCE PREFLIGHT BOOT"));
  Serial.println(
      F("READ-ONLY: no flash program/erase path is linked into this image"));
  Serial.println(
      F("REGION 0x0E5000..0x0E6FFF; qualification requires both pages all_ff=yes"));

  printPage(0);
  printPage(1);

  uint32_t ignored_offset = 0;
  uint8_t ignored_value = 0;
  const bool page_a_ff = pageAllFf(0, ignored_offset, ignored_value);
  const bool page_b_ff = pageAllFf(1, ignored_offset, ignored_value);
  Serial.printf("M6D3B PREFLIGHT RESULT all_ff=%s action=%s\n",
                (page_a_ff && page_b_ff) ? "yes" : "no",
                (page_a_ff && page_b_ff)
                    ? "QUALIFICATION_IMAGE_MAY_BE_USED"
                    : "STOP_DO_NOT_ERASE");
  Serial.flush();
}

void loop() {
  delay(1000);
}
