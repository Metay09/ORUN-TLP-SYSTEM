// SF5D4 TEST-ONLY. Writes a 16-byte marker at the start of every fully erased
// page of the unassigned candidate [0x0C5000, 0x0E5000). Never erases, never
// touches a page that holds any data, and only after an explicit serial
// command. Upload only to an owner-approved RAK4631 development device.
#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <nrf_sdm.h>
#include <nrf_soc.h>
#include <nrf.h>
#include <string.h>

#include "sf5d_full_region_shared.h"

using namespace sf5d_full_region;

namespace {
char command[40]{};
unsigned command_length = 0U;

bool physicalSafety() {
  uint8_t enabled = 1;
  const uint32_t bootloader = NRF_UICR->NRFFW[0];
  return NRF_FICR->CODEPAGESIZE == kPageSize &&
         NRF_FICR->CODESIZE == 256U &&
         (bootloader == UINT32_MAX || bootloader >= 0x0F4000U) &&
         sd_softdevice_is_enabled(&enabled) == NRF_SUCCESS && enabled == 0;
}

void seedAll() {
  if (!physicalSafety()) {
    Serial.println(F("SF5D FULL SEED FAIL reason=flash-geometry-or-softdevice"));
    return;
  }
  unsigned written = 0, kept = 0;
  for (unsigned i = 0; i < kPages; ++i) {
    feedInheritedWatchdog();
    const uint32_t address = pageAddress(i);
    if (!pageErased(address)) {  // marker or other data: read-only control
      ++kept;
      continue;
    }
    alignas(4) uint32_t marker[4]{};
    expectedWords(address, marker);
    if (sd_flash_write(reinterpret_cast<uint32_t*>(address), marker, 4U) !=
            NRF_SUCCESS ||
        !markerMatches(address)) {
      Serial.printf("SF5D FULL SEED FAIL address=0x%06lX\n",
                    static_cast<unsigned long>(address));
      printFullStatus();
      return;
    }
    ++written;
  }
  Serial.printf("SF5D FULL SEED PASS written=%u kept=%u\n", written, kept);
  printFullStatus();
}

void handleCommand() {
  if (acceptLine(command, "SF5D FULL STATUS")) {
    printFullStatus();
  } else if (acceptLine(command, "SF5D FULL SEED CONFIRM")) {
    seedAll();
  } else {
    Serial.println(F("SF5D FULL COMMAND rejected; use SF5D FULL STATUS or "
                     "SF5D FULL SEED CONFIRM"));
  }
  command_length = 0U;
  command[0] = '\0';
}
}  // namespace

void setup() {
  Serial.begin(115200);
  const uint32_t start = millis();
  while (!Serial && (millis() - start) < 15000U) {
    feedInheritedWatchdog();
    delay(10);
  }
  Serial.println(F("SF5D FULL SEED TEST-ONLY; NO AUTOMATIC MUTATION"));
  printFullStatus();
}

void loop() {
  feedInheritedWatchdog();
  for (unsigned i = 0; i < 40U && Serial.available(); ++i) {
    const int c = Serial.read();
    if (c < 0) break;
    if (c == '\r' || c == '\n') {
      if (command_length == 0U) continue;
      command[command_length] = '\0';
      handleCommand();
    } else if (command_length < sizeof(command) - 1U) {
      command[command_length++] = static_cast<char>(c);
    } else {
      command_length = 0U;
      command[0] = '\0';
      Serial.println(F("SF5D FULL COMMAND rejected length"));
    }
  }
}
