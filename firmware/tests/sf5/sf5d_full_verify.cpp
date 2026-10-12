// SF5D4 TEST-ONLY read-only verification of the product region [0x0C5000,
// 0x0E5000). Links no flash write or erase; it only prints the page states,
// the 32 page CRCs and their digest.
#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <nrf.h>
#include <string.h>

#include "sf5d_full_region_shared.h"

using namespace sf5d_full_region;

namespace {
char command[40]{};
unsigned command_length = 0U;
}

void setup() {
  Serial.begin(115200);
  const uint32_t start = millis();
  while (!Serial && (millis() - start) < 15000U) {
    feedInheritedWatchdog();
    delay(10);
  }
  Serial.println(F("SF5D FULL VERIFY TEST-ONLY READ-ONLY"));
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
      if (acceptLine(command, "SF5D FULL STATUS"))
        printFullStatus();
      else
        Serial.println(F("SF5D FULL VERIFY READ-ONLY; use SF5D FULL STATUS"));
      command_length = 0U;
      command[0] = '\0';
    } else if (command_length < sizeof(command) - 1U) {
      command[command_length++] = static_cast<char>(c);
    } else {
      command_length = 0U;
      command[0] = '\0';
      Serial.println(F("SF5D FULL COMMAND rejected length"));
    }
  }
}
