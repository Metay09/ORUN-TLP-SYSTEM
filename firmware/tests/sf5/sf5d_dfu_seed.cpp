// SF5D TEST-ONLY. Physically writes one 16-byte sentinel at 0x0E4000.
// Upload only to the owner-approved RAK4631 development device. Explicit
// serial command required; no boot-time auto erase or write.
#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <nrf_sdm.h>
#include <nrf_soc.h>
#include <nrf.h>
#include <string.h>

#include "sf5d_dfu_sentinel_shared.h"

using namespace sf5d_dfu_sentinel;

namespace {
char command[32]{};
unsigned command_length = 0U;

bool physicalSafety() {
  uint8_t enabled = 1;
  const uint32_t bootloader = NRF_UICR->NRFFW[0];
  return NRF_FICR->CODEPAGESIZE == kPageSize &&
         NRF_FICR->CODESIZE == 256U &&
         (bootloader == UINT32_MAX || bootloader >= 0x0F4000U) &&
         sd_softdevice_is_enabled(&enabled) == NRF_SUCCESS && enabled == 0;
}

void seedOnce() {
  if (!physicalSafety()) {
    Serial.println(F("SF5D DFU SEED FAIL reason=flash-geometry-or-softdevice"));
    return;
  }
  // The two previously observed nonblank pages (0xC5000, 0xD5000) are
  // read-only reference controls. Do NOT erase or program either page.
  if (!pageErased(kSeedAddress)) {
    Serial.printf("SF5D DFU SEED REJECT non-erased-page=0x%06lX\n",
                  static_cast<unsigned long>(kSeedAddress));
    printStatus();
    return;
  }
  alignas(4) uint32_t marker[4]{};
  expectedWords(kSeedAddress, marker);
  if (sd_flash_write(reinterpret_cast<uint32_t*>(kSeedAddress),
                     marker, 4U) != NRF_SUCCESS ||
      !markerMatches(kSeedAddress)) {
    Serial.printf("SF5D DFU SEED FAIL address=0x%06lX\n",
                  static_cast<unsigned long>(kSeedAddress));
    printStatus();
    return;
  }
  feedInheritedWatchdog();
  Serial.println(F("SF5D DFU SEED PASS one-page-readback"));
  printStatus();
}

void handleCommand() {
  if (acceptLine(command, "SF5D STATUS")) {
    printStatus();
  } else if (acceptLine(command, "SF5D SEED CONFIRM")) {
    seedOnce();
  } else {
    Serial.println(F("SF5D DFU COMMAND rejected; use SF5D STATUS or SF5D SEED CONFIRM"));
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
  Serial.println(F("SF5D DFU SEED TEST-ONLY; NO AUTOMATIC MUTATION"));
  printStatus();
}

void loop() {
  feedInheritedWatchdog();
  for (unsigned i = 0; i < 32U && Serial.available(); ++i) {
    const int c = Serial.read();
    if (c < 0) break;
    if (c == '\r' || c == '\n') {
      if (command_length == 0U) continue;  // Ignore a trailing CR/LF.
      command[command_length] = '\0';
      handleCommand();
    } else if (command_length < sizeof(command) - 1U) {
      command[command_length++] = static_cast<char>(c);
    } else {
      command_length = 0U;
      command[0] = '\0';
      Serial.println(F("SF5D DFU COMMAND rejected length"));
    }
  }
}
