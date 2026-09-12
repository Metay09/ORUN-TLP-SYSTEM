#include <Arduino.h>
#include <Adafruit_TinyUSB.h>

#include "firmware_version.h"

namespace {

void printBootBanner() {
  Serial.println(F("ORUN TLP"));
  Serial.print(F("firmware version "));
  Serial.println(orun_tlp::kFirmwareVersion);
  Serial.print(F("build date/time "));
  Serial.print(F(__DATE__));
  Serial.print(F(" "));
  Serial.println(F(__TIME__));
  Serial.println(F("M0 BOOT OK"));
}

}  // namespace

void setup() {
  Serial.begin(115200);
  printBootBanner();
}

void loop() {
}
