// M7P6F TEST-ONLY SecurityStore cleanup helper.
//
// Destructive scope: ONLY the two-page SecurityStore partition
// 0x0E7000..0x0E9000. Use only on a development unit after M7P6F physical
// sentinel work. It never touches ConfigStore, bonds/InternalFS, History,
// bootloader/settings, radio/GNSS state, or application flash.
//
// The helper requires an explicit serial CLEAN command, erases both security
// pages synchronously with SoftDevice disabled, verifies the complete region
// reads back as 0xFF, and then repeats a terminal PASS/FAIL line.
//
// Never flash this image to a deployed/provisioned node.
#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <nrf_sdm.h>

#include <string.h>

#include "flash_backend.h"
#include "storage_config.h"

using namespace orun_tlp;
using namespace orun_tlp::storage_config;

namespace {

NrfSecurityFlash security_flash;
bool done = false;
uint32_t last_report_ms = 0;
uint32_t last_ready_report_ms = 0;
char command_buffer[16]{};
uint8_t command_length = 0;
char final_report[128] = "M7P6F SECURITY CLEAN NOT RUN";

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

bool regionErased() {
  constexpr uint32_t kRegionSize =
      kFutureSecurityRegionEnd - kFutureSecurityRegionStart;
  uint8_t bytes[64];

  for (uint32_t offset = 0; offset < kRegionSize; offset += sizeof(bytes)) {
    const size_t remaining = kRegionSize - offset;
    const size_t chunk = remaining < sizeof(bytes) ? remaining : sizeof(bytes);
    if (!security_flash.read(offset, bytes, chunk)) return false;
    for (size_t i = 0; i < chunk; ++i) {
      if (bytes[i] != 0xFFU) return false;
    }
  }
  return true;
}

void cleanSecurityPartition() {
  Serial.println(F("M7P6F SECURITY CLEAN accepted"));
  Serial.flush();

  for (uint32_t page = 0; page < kFutureSecurityRegionPages; ++page) {
    if (security_flash.erasePage(page) != FlashOpResult::kDone) {
      setReport("M7P6F SECURITY CLEAN FAIL erase");
      return;
    }
  }

  if (!regionErased()) {
    setReport("M7P6F SECURITY CLEAN FAIL verify");
    return;
  }

  setReport("M7P6F SECURITY CLEAN PASS pages=2 all_ff=yes");
}

}  // namespace

void setup() {
  Serial.begin(115200);
  const uint32_t wait_started = millis();
  while (!Serial && (millis() - wait_started) < 15000U) delay(10);

  Serial.println(F("M7P6F SECURITY CLEANUP BOOT"));
  Serial.println(F("TEST-ONLY: destructively owns ONLY SecurityStore 0xE7000..0xE9000"));
  Serial.flush();

  if (!softDeviceDisabled()) {
    setReport("M7P6F SECURITY CLEAN FAIL softdevice_enabled");
    return;
  }

  if (!security_flash.begin()) {
    setReport("M7P6F SECURITY CLEAN FAIL security_flash_begin");
    return;
  }

  Serial.println(F("M7P6F SECURITY CLEAN READY send CLEAN"));
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

  if (Serial && (millis() - last_ready_report_ms) >= 3000U) {
    Serial.println(F("M7P6F SECURITY CLEAN READY send CLEAN"));
    Serial.flush();
    last_ready_report_ms = millis();
  }

  while (Serial.available() > 0) {
    const char ch = static_cast<char>(Serial.read());
    if (ch == '\r') continue;

    if (ch == '\n') {
      command_buffer[command_length] = '\0';
      if (strcmp(command_buffer, "CLEAN") == 0) {
        cleanSecurityPartition();
      } else if (command_length != 0) {
        Serial.println(F("M7P6F SECURITY CLEAN command rejected; send CLEAN"));
        Serial.flush();
      }
      command_length = 0;
      break;
    }

    if (command_length + 1U < sizeof(command_buffer)) {
      command_buffer[command_length++] = ch;
    } else {
      command_length = 0;
      Serial.println(F("M7P6F SECURITY CLEAN command too long; send CLEAN"));
      Serial.flush();
    }
  }

  delay(20);
}
