// SF5D TEST-ONLY read-only DFU retention verification.
// This image does not call sd_flash_write/page_erase and cannot seed flash.
#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <nrf.h>
#include <string.h>

#include "sf5d_dfu_sentinel_shared.h"

using namespace sf5d_dfu_sentinel;

namespace {
char command[32]{};
unsigned command_length = 0U;
}

void setup() {
  Serial.begin(115200);
  const uint32_t start = millis();
  while (!Serial && (millis() - start) < 15000U) {
    feedInheritedWatchdog();
    delay(10);
  }
  Serial.println(F("SF5D DFU VERIFY TEST-ONLY READ-ONLY"));
  printStatus();
}

void loop() {
  feedInheritedWatchdog();
  for (unsigned i = 0; i < 32U && Serial.available(); ++i) {
    const int c = Serial.read();
    if (c < 0) break;
    if (c == '\r' || c == '\n') {
      command[command_length] = '\0';
      if (acceptLine(command, "SF5D STATUS")) {
        printStatus();
      } else {
        Serial.println(F("SF5D DFU VERIFY READ-ONLY; use SF5D STATUS"));
      }
      command_length = 0U;
      command[0] = '\0';
    } else if (command_length < sizeof(command) - 1U) {
      command[command_length++] = static_cast<char>(c);
    } else {
      command_length = 0U;
      command[0] = '\0';
      Serial.println(F("SF5D DFU COMMAND rejected length"));
    }
  }
}
