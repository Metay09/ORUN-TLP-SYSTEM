#include <Arduino.h>
#include <Adafruit_TinyUSB.h>

#include "firmware_version.h"
#include "gnss_manager.h"
#include "radio_manager.h"

namespace {

orun_tlp::RadioManager radio_manager;
orun_tlp::GnssManager gnss_manager;

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
  radio_manager.begin();
  gnss_manager.begin();
}

void loop() {
  radio_manager.update();
  gnss_manager.poll();
  orun_tlp::GnssFix fix{};
  if (radio_manager.canSend() && gnss_manager.takeFreshFixForTransmission(&fix)) {
    radio_manager.sendPosition(fix);
  }
  delay(10);
}
