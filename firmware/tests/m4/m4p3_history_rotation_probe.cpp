// M4P3 TEST-ONLY History v4 record-driven page-rotation probe.
//
// Boot/recovery is read-only. No mutation occurs until the operator sends the
// exact serial command ROTATE.
//
// ROTATE uses the real HistoryStore + NrfHistoryFlash synchronous path to append
// synthetic but valid TLP v1 POSITION records until a later-generation History
// page is physically committed. The probe then requires the rotated page to
// carry the exact incarnation recovered from the existing v4 stream.
//
// This probe is destructive only in the sense that it intentionally adds test
// History records to a development unit. It never erases/re-baselines History,
// Config, Security, Geofence, bonds/InternalFS, bootloader/settings or app flash.
//
// Never deploy this image.
#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <nrf_sdm.h>
#include <nrf.h>

#include <stdio.h>
#include <string.h>

#include "flash_backend.h"
#include "history_store.h"
#include "journal_format.h"
#include "storage_config.h"
#include "tlp_position_packet.h"

using namespace orun_tlp;
using namespace orun_tlp::journal_format;
using namespace orun_tlp::storage_config;

namespace {

bool inheritedWatchdogRunning() {
  return NRF_WDT->RUNSTATUS != 0;
}

void feedInheritedWatchdog() {
  if (inheritedWatchdogRunning())
    NRF_WDT->RR[0] = WDT_RR_RR_Reload;
}

class RejectingIncarnationSource : public HistoryIncarnationSource {
 public:
  bool generate(uint64_t& incarnation) override {
    ++calls;
    incarnation = 0;
    return false;
  }
  uint32_t calls = 0;
};

NrfHistoryFlash history_flash;
RejectingIncarnationSource rejecting_source;
HistoryStore history(history_flash, &rejecting_source);

bool probe_ready = false;
bool terminal = false;
uint64_t device_id = 0;
uint64_t baseline_incarnation = 0;
uint64_t baseline_generation = 0;
uint32_t baseline_page = UINT32_MAX;
uint32_t last_ready_report_ms = 0;
uint32_t last_terminal_report_ms = 0;
char command_buffer[20]{};
uint8_t command_length = 0;
char final_report[192] = "M4P3 ROTATION PROBE NOT TERMINAL";

bool softDeviceDisabled() {
  uint8_t enabled = 1;
  return sd_softdevice_is_enabled(&enabled) == NRF_SUCCESS && enabled == 0;
}

bool committedEnvelope(const uint8_t* header) {
  return get32(header) == kPageMagic &&
         get32(header + 60) == kCommit &&
         get32(header + 56) == crc32(header, 56);
}

bool readValidV4Page(uint32_t page, uint64_t expected_device,
                     uint64_t& generation, uint64_t& incarnation) {
  uint8_t header[kStaticHeaderSize]{};
  if (!history_flash.read(page * kPageSize, header, sizeof(header)))
    return false;
  if (!committedEnvelope(header) || header[4] != kVersion)
    return false;
  const uint64_t header_device = get64(header + 16);
  if (expected_device != 0 && header_device != expected_device)
    return false;
  const uint64_t decode_device =
      expected_device != 0 ? expected_device : header_device;
  return decodePage(header, decode_device, generation, incarnation);
}

bool discoverBaseline() {
  bool found = false;
  for (uint32_t page = 0; page < kPageCount; ++page) {
    uint8_t header[kStaticHeaderSize]{};
    if (!history_flash.read(page * kPageSize, header, sizeof(header)))
      return false;
    if (!committedEnvelope(header) || header[4] != kVersion)
      continue;

    const uint64_t candidate_device = get64(header + 16);
    uint64_t generation = 0;
    uint64_t incarnation = 0;
    if (!decodePage(header, candidate_device, generation, incarnation))
      continue;

    if (!found || generation > baseline_generation) {
      found = true;
      device_id = candidate_device;
      baseline_generation = generation;
      baseline_incarnation = incarnation;
      baseline_page = page;
    }
  }
  return found && device_id != 0 && baseline_incarnation != 0;
}

void settleStore() {
  while (history.busy()) {
    feedInheritedWatchdog();
    history.poll();
  }
}

bool allocateRecord(HistoryStore::Record& record, uint32_t synthetic_index) {
  uint32_t sequence = 0;
  for (unsigned attempt = 0; attempt < 4; ++attempt) {
    if (history.nextSequence(sequence, record.identity))
      break;
    if (!history.busy())
      return false;
    settleStore();
  }
  if (record.identity == 0)
    return false;

  const tlp::PositionPacket packet{
      device_id,
      sequence,
      0,
      static_cast<int32_t>(376100000 + synthetic_index),
      static_cast<int32_t>(280540000 + synthetic_index),
      0,
      100,
      8,
      tlp::kPositionFlagValidFix};

  return tlp::serializePositionPacket(packet, record.packet,
                                      sizeof(record.packet));
}

bool appendRecord(uint32_t synthetic_index) {
  HistoryStore::Record record;
  if (!allocateRecord(record, synthetic_index))
    return false;
  if (!history.append(record.packet, record.identity))
    return false;
  settleStore();
  bool success = false;
  return history.takeAppendResult(success) && success;
}

bool findRotatedPage(uint32_t& page_out, uint64_t& generation_out,
                     uint64_t& incarnation_out) {
  for (uint32_t page = 0; page < kPageCount; ++page) {
    uint64_t generation = 0;
    uint64_t incarnation = 0;
    if (!readValidV4Page(page, device_id, generation, incarnation))
      continue;
    if (generation > baseline_generation) {
      page_out = page;
      generation_out = generation;
      incarnation_out = incarnation;
      return true;
    }
  }
  return false;
}

void printHex64(uint64_t value) {
  Serial.printf("%08lX%08lX",
                static_cast<unsigned long>(static_cast<uint32_t>(value >> 32)),
                static_cast<unsigned long>(static_cast<uint32_t>(value)));
}

void printStatus() {
  Serial.printf("M4P3 ROTATION STORE ready=%s busy=%s count=%lu source_calls=%lu\n",
                history.ready() ? "yes" : "no",
                history.busy() ? "yes" : "no",
                static_cast<unsigned long>(history.count()),
                static_cast<unsigned long>(rejecting_source.calls));
  Serial.print(F("M4P3 ROTATION BASE page="));
  Serial.print(static_cast<unsigned long>(baseline_page));
  Serial.print(F(" generation=0x"));
  printHex64(baseline_generation);
  Serial.print(F(" incarnation=0x"));
  printHex64(baseline_incarnation);
  Serial.println();

  for (uint32_t page = 0; page < kPageCount; ++page) {
    uint64_t generation = 0;
    uint64_t incarnation = 0;
    if (readValidV4Page(page, device_id, generation, incarnation)) {
      Serial.printf("M4P3 ROTATION PAGE %lu VALID generation=0x",
                    static_cast<unsigned long>(page));
      printHex64(generation);
      Serial.print(F(" incarnation=0x"));
      printHex64(incarnation);
      Serial.println();
    }
  }
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

void runRotation() {
  Serial.println(F("M4P3 ROTATION accepted: appending test POSITION records"));
  Serial.flush();

  if (!probe_ready || !history.ready() || history.busy()) {
    setTerminalReport("M4P3 ROTATION FAIL store_not_ready");
    return;
  }
  if (history.incarnation() != baseline_incarnation ||
      baseline_incarnation == 0 || rejecting_source.calls != 0) {
    setTerminalReport("M4P3 ROTATION FAIL baseline_invariant");
    return;
  }

  uint32_t rotated_page = UINT32_MAX;
  uint64_t rotated_generation = 0;
  uint64_t rotated_incarnation = 0;
  uint32_t appended = 0;

  // One existing page can hold 104 records. A maximum of 105 appends is
  // sufficient even if the page started empty; the normal record-driven path
  // must then create a later-generation page.
  while (appended <= kRecordsPerPage) {
    feedInheritedWatchdog();
    if (findRotatedPage(rotated_page, rotated_generation,
                        rotated_incarnation))
      break;

    if (!appendRecord(appended)) {
      char report[192];
      snprintf(report, sizeof(report),
               "M4P3 ROTATION FAIL append index=%lu count=%lu",
               static_cast<unsigned long>(appended),
               static_cast<unsigned long>(history.count()));
      setTerminalReport(report);
      return;
    }
    ++appended;

    if ((appended % 16U) == 0U) {
      Serial.printf("M4P3 ROTATION progress appended=%lu count=%lu\n",
                    static_cast<unsigned long>(appended),
                    static_cast<unsigned long>(history.count()));
      Serial.flush();
    }
  }

  if (!findRotatedPage(rotated_page, rotated_generation,
                       rotated_incarnation)) {
    setTerminalReport("M4P3 ROTATION FAIL no_later_generation_page");
    return;
  }
  if (rotated_incarnation != baseline_incarnation ||
      rotated_generation <= baseline_generation ||
      rejecting_source.calls != 0) {
    setTerminalReport("M4P3 ROTATION FAIL incarnation_or_generation");
    return;
  }

  char report[192];
  snprintf(report, sizeof(report),
           "M4P3 ROTATION PASS appended=%lu page=%lu generation=%08lX%08lX "
           "incarnation=%08lX%08lX source_calls=%lu",
           static_cast<unsigned long>(appended),
           static_cast<unsigned long>(rotated_page),
           static_cast<unsigned long>(
               static_cast<uint32_t>(rotated_generation >> 32)),
           static_cast<unsigned long>(
               static_cast<uint32_t>(rotated_generation)),
           static_cast<unsigned long>(
               static_cast<uint32_t>(rotated_incarnation >> 32)),
           static_cast<unsigned long>(
               static_cast<uint32_t>(rotated_incarnation)),
           static_cast<unsigned long>(rejecting_source.calls));
  setTerminalReport(report);
}

void handleCommand() {
  command_buffer[command_length] = '\0';

  if (strcmp(command_buffer, "STATUS") == 0) {
    printStatus();
  } else if (strcmp(command_buffer, "ROTATE") == 0) {
    runRotation();
  } else if (command_length != 0) {
    Serial.println(F("M4P3 ROTATION command rejected; use STATUS or ROTATE"));
    Serial.flush();
  }
  command_length = 0;
}

}  // namespace

void setup() {
  const bool inherited_watchdog = inheritedWatchdogRunning();
  feedInheritedWatchdog();
  Serial.begin(115200);
  const uint32_t started = millis();
  while (!Serial && (millis() - started) < 15000U) {
    feedInheritedWatchdog();
    delay(10);
  }

  Serial.printf("M4P3 HISTORY V4 ROTATION PROBE BOOT inherited_watchdog=%s\n",
                inherited_watchdog ? "yes" : "no");
  Serial.println(F("TEST-ONLY: boot is read-only; ROTATE adds History test records"));
  Serial.println(F("RULE: do not send ROTATE without explicit operator approval"));
  Serial.flush();

  if (!softDeviceDisabled()) {
    setTerminalReport("M4P3 ROTATION FAIL softdevice_enabled");
    return;
  }
  if (!history_flash.begin()) {
    setTerminalReport("M4P3 ROTATION FAIL history_flash_begin");
    return;
  }
  if (!discoverBaseline()) {
    setTerminalReport("M4P3 ROTATION FAIL no_valid_v4_baseline");
    return;
  }
  if (!history.begin(device_id)) {
    setTerminalReport("M4P3 ROTATION FAIL history_begin");
    return;
  }
  settleStore();

  if (!history.ready() ||
      history.incarnation() != baseline_incarnation ||
      rejecting_source.calls != 0) {
    setTerminalReport("M4P3 ROTATION FAIL recovery_invariant");
    return;
  }

  probe_ready = true;
  printStatus();
  Serial.println(F("M4P3 ROTATION READY commands=STATUS,ROTATE"));
  Serial.flush();
  last_ready_report_ms = millis();
}

void loop() {
  feedInheritedWatchdog();
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
    Serial.println(F("M4P3 ROTATION READY commands=STATUS,ROTATE"));
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
      Serial.println(F("M4P3 ROTATION command too long; use STATUS or ROTATE"));
      Serial.flush();
    }
  }
  delay(20);
}
