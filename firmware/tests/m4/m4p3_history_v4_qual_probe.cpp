// M4P3 TEST-ONLY History v4 physical qualification helper.
//
// Purpose:
// - read-only classify every History page before destructive v3 -> v4 cutover;
// - allow an explicit CLEAN command that erases ONLY History
//   0x0ED000..0x0F3FFF;
// - read-verify the complete 28 KiB region as 0xFF after erase.
//
// This image does NOT construct HistoryStore and therefore never creates a v4
// stream by itself. After CLEAN PASS, power-cycle and flash the normal
// production v4 image to establish the fresh incarnation.
//
// Never deploy this image to a production unit.
#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <nrf_sdm.h>

#include <stdio.h>
#include <string.h>

#include "flash_backend.h"
#include "journal_format.h"
#include "storage_config.h"

using namespace orun_tlp;
using namespace orun_tlp::journal_format;
using namespace orun_tlp::storage_config;

namespace {

NrfHistoryFlash history_flash;

bool terminal = false;
uint32_t last_terminal_report_ms = 0;
uint32_t last_ready_report_ms = 0;
char command_buffer[20]{};
uint8_t command_length = 0;
char final_report[160] = "M4P3 HISTORY QUAL NOT TERMINAL";

enum class HeaderEvidence : uint8_t {
  kErased,
  kCommittedV2,
  kCommittedV3,
  kCommittedV4Valid,
  kCommittedV4Invalid,
  kCommittedFuture,
  kTornOrDebris,
};

const char* evidenceName(HeaderEvidence evidence) {
  switch (evidence) {
    case HeaderEvidence::kErased: return "ERASED";
    case HeaderEvidence::kCommittedV2: return "COMMITTED_V2";
    case HeaderEvidence::kCommittedV3: return "COMMITTED_V3";
    case HeaderEvidence::kCommittedV4Valid: return "COMMITTED_V4_VALID";
    case HeaderEvidence::kCommittedV4Invalid: return "COMMITTED_V4_INVALID";
    case HeaderEvidence::kCommittedFuture: return "COMMITTED_FUTURE";
    case HeaderEvidence::kTornOrDebris: return "TORN_OR_DEBRIS";
  }
  return "UNKNOWN";
}

bool softDeviceDisabled() {
  uint8_t enabled = 1;
  return sd_softdevice_is_enabled(&enabled) == NRF_SUCCESS && enabled == 0;
}

bool committedEnvelope(const uint8_t* header) {
  return get32(header) == kPageMagic &&
         get32(header + 60) == kCommit &&
         get32(header + 56) == crc32(header, 56);
}

bool allFf(const uint8_t* bytes, size_t size) {
  for (size_t i = 0; i < size; ++i)
    if (bytes[i] != 0xFFU) return false;
  return true;
}

HeaderEvidence classifyHeader(const uint8_t* header,
                              uint64_t& generation,
                              uint64_t& incarnation) {
  generation = 0;
  incarnation = 0;

  if (allFf(header, kStaticHeaderSize))
    return HeaderEvidence::kErased;

  if (!committedEnvelope(header))
    return HeaderEvidence::kTornOrDebris;

  const uint8_t version = header[4];
  if (version == 2) return HeaderEvidence::kCommittedV2;
  if (version == 3) return HeaderEvidence::kCommittedV3;
  if (version > kVersion) return HeaderEvidence::kCommittedFuture;
  if (version != kVersion) return HeaderEvidence::kTornOrDebris;

  return decodePage(header, get64(header + 16), generation, incarnation)
             ? HeaderEvidence::kCommittedV4Valid
             : HeaderEvidence::kCommittedV4Invalid;
}

bool pageAllFf(uint32_t page, uint32_t& first_non_ff_offset,
               uint8_t& first_non_ff_value) {
  uint8_t bytes[64];
  for (uint32_t offset = 0; offset < kPageSize; offset += sizeof(bytes)) {
    const size_t remaining = kPageSize - offset;
    const size_t chunk = remaining < sizeof(bytes) ? remaining : sizeof(bytes);
    if (!history_flash.read(page * kPageSize + offset, bytes, chunk))
      return false;
    for (size_t i = 0; i < chunk; ++i) {
      if (bytes[i] != 0xFFU) {
        first_non_ff_offset = offset + i;
        first_non_ff_value = bytes[i];
        return false;
      }
    }
  }
  first_non_ff_offset = UINT32_MAX;
  first_non_ff_value = 0xFF;
  return true;
}

bool regionErased() {
  uint8_t bytes[64];
  for (uint32_t offset = 0; offset < kRegionSize; offset += sizeof(bytes)) {
    const size_t remaining = kRegionSize - offset;
    const size_t chunk = remaining < sizeof(bytes) ? remaining : sizeof(bytes);
    if (!history_flash.read(offset, bytes, chunk)) return false;
    if (!allFf(bytes, chunk)) return false;
  }
  return true;
}

void printU64Hex(uint64_t value) {
  Serial.printf("%08lX%08lX",
                static_cast<unsigned long>(static_cast<uint32_t>(value >> 32)),
                static_cast<unsigned long>(static_cast<uint32_t>(value)));
}

void printPageStatus(uint32_t page) {
  uint8_t header[kStaticHeaderSize];
  if (!history_flash.read(page * kPageSize, header, sizeof(header))) {
    Serial.printf("M4P3 HISTORY PAGE %lu read=FAIL\n",
                  static_cast<unsigned long>(page));
    return;
  }

  uint64_t generation = 0;
  uint64_t incarnation = 0;
  const HeaderEvidence evidence =
      classifyHeader(header, generation, incarnation);

  uint32_t first_non_ff_offset = UINT32_MAX;
  uint8_t first_non_ff_value = 0xFF;
  const bool all_ff =
      pageAllFf(page, first_non_ff_offset, first_non_ff_value);

  Serial.printf("M4P3 HISTORY PAGE %lu evidence=%s all_ff=%s",
                static_cast<unsigned long>(page),
                evidenceName(evidence),
                all_ff ? "yes" : "no");

  if (evidence == HeaderEvidence::kCommittedV4Valid) {
    Serial.print(F(" generation=0x"));
    printU64Hex(generation);
    Serial.print(F(" incarnation=0x"));
    printU64Hex(incarnation);
  } else if (committedEnvelope(header)) {
    Serial.printf(" version=%u", static_cast<unsigned>(header[4]));
  }

  if (!all_ff) {
    Serial.printf(" first_non_ff=0x%04lX value=%02X",
                  static_cast<unsigned long>(first_non_ff_offset),
                  static_cast<unsigned>(first_non_ff_value));
  }
  Serial.println();
}

void printStatus() {
  Serial.printf("M4P3 HISTORY REGION 0x%05lX..0x%05lX pages=%lu bytes=%lu\n",
                static_cast<unsigned long>(kBaseAddress),
                static_cast<unsigned long>(kBaseAddress + kRegionSize - 1),
                static_cast<unsigned long>(kPageCount),
                static_cast<unsigned long>(kRegionSize));

  for (uint32_t page = 0; page < kPageCount; ++page)
    printPageStatus(page);

  Serial.printf("M4P3 HISTORY STATUS region_all_ff=%s\n",
                regionErased() ? "yes" : "no");
  Serial.flush();
}

void setTerminalReport(const char* text) {
  strncpy(final_report, text, sizeof(final_report) - 1);
  final_report[sizeof(final_report) - 1] = '\0';
  Serial.println(final_report);
  Serial.flush();
  last_terminal_report_ms = millis();
  terminal = true;
}

void cleanHistoryPartition() {
  Serial.println(
      F("M4P3 HISTORY CLEAN accepted scope=0x0ED000..0x0F3FFF pages=7"));
  Serial.flush();

  if (!softDeviceDisabled()) {
    setTerminalReport(
        "M4P3 HISTORY CLEAN FAIL softdevice_enabled; POWER-CYCLE BEFORE RETRY");
    return;
  }
  if (!history_flash.begin()) {
    setTerminalReport(
        "M4P3 HISTORY CLEAN FAIL history_flash_begin; POWER-CYCLE BEFORE RETRY");
    return;
  }

  for (uint32_t page = 0; page < kPageCount; ++page) {
    if (history_flash.erasePage(page) != FlashOpResult::kDone) {
      char report[160];
      snprintf(report, sizeof(report),
               "M4P3 HISTORY CLEAN FAIL erase page=%lu; POWER-CYCLE BEFORE RETRY",
               static_cast<unsigned long>(page));
      setTerminalReport(report);
      return;
    }
  }

  if (!regionErased()) {
    setTerminalReport(
        "M4P3 HISTORY CLEAN FAIL verify; POWER-CYCLE BEFORE RETRY");
    return;
  }

  setTerminalReport(
      "M4P3 HISTORY CLEAN PASS pages=7 bytes=28672 all_ff=yes; POWER-CYCLE NOW");
}

void handleCommand() {
  command_buffer[command_length] = '\0';

  if (strcmp(command_buffer, "STATUS") == 0) {
    printStatus();
  } else if (strcmp(command_buffer, "CLEAN") == 0) {
    cleanHistoryPartition();
  } else if (command_length != 0) {
    Serial.println(F("M4P3 HISTORY QUAL command rejected; use STATUS or CLEAN"));
    Serial.flush();
  }

  command_length = 0;
}

}  // namespace

void setup() {
  Serial.begin(115200);
  const uint32_t wait_started = millis();
  while (!Serial && (millis() - wait_started) < 15000U) delay(10);

  Serial.println(F("M4P3 HISTORY V4 PHYSICAL QUAL BOOT"));
  Serial.println(
      F("TEST-ONLY: CLEAN destructively owns ONLY History 0x0ED000..0x0F3FFF"));
  Serial.println(
      F("RULE: inspect STATUS first; do not send CLEAN without explicit operator approval"));
  Serial.flush();

  if (!softDeviceDisabled()) {
    setTerminalReport("M4P3 HISTORY QUAL FAIL softdevice_enabled");
    return;
  }

  if (!history_flash.begin()) {
    setTerminalReport("M4P3 HISTORY QUAL FAIL history_flash_begin");
    return;
  }

  printStatus();
  Serial.println(F("M4P3 HISTORY QUAL READY commands=STATUS,CLEAN"));
  Serial.flush();
  last_ready_report_ms = millis();
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

  if (Serial && (millis() - last_ready_report_ms) >= 5000U) {
    Serial.println(F("M4P3 HISTORY QUAL READY commands=STATUS,CLEAN"));
    Serial.flush();
    last_ready_report_ms = millis();
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
      Serial.println(F("M4P3 HISTORY QUAL command too long; use STATUS or CLEAN"));
      Serial.flush();
    }
  }

  delay(20);
}
