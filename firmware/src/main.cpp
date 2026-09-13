#include <Arduino.h>
#include <Adafruit_TinyUSB.h>

#include "firmware_version.h"
#include "gnss_manager.h"
#include "power_manager.h"
#include "radio_manager.h"
#include "position_flow.h"
#include "monotonic_time.h"

namespace {

orun_tlp::RadioManager radio_manager;
orun_tlp::GnssManager gnss_manager;
orun_tlp::NrfHistoryFlash history_flash;
orun_tlp::HistoryStore history(history_flash);
orun_tlp::PositionFlow positions(history, radio_manager);

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
  radio_manager.begin(history);
  if (history.begin(radio_manager.deviceId())) {
    Serial.printf("STORAGE recovered records=%lu capacity=%lu corrupt=%lu pending=%lu\n",
                  static_cast<unsigned long>(history.count()),
                  static_cast<unsigned long>(history.capacity()),
                  static_cast<unsigned long>(history.diagnostics().recovery_corruptions),
                  static_cast<unsigned long>(history.backlogCount()));
  } else Serial.println(F("STORAGE unavailable; POSITION TX disabled"));
  gnss_manager.begin();
}

void loop() {
  gnss_manager.poll();
  // Leave local TX undisturbed; otherwise service one small flash operation.
  if (!radio_manager.isTransmitting()) history.poll();
  const auto event = positions.update(orun_tlp::monotonic::nowMs());
  if (event == orun_tlp::PositionFlow::Event::kStorageFailure)
    Serial.println(F("STORAGE append failed; no live TX"));
  else if (event == orun_tlp::PositionFlow::Event::kStored)
    Serial.printf("STORAGE appended records=%lu overwritten=%lu pending=%lu\n",
                  static_cast<unsigned long>(history.count()),
                  static_cast<unsigned long>(history.diagnostics().overwritten),
                  static_cast<unsigned long>(history.backlogCount()));
  else if (event == orun_tlp::PositionFlow::Event::kLiveExpired)
    Serial.println(F("POSITION live expired; retained in history"));
  orun_tlp::GnssFix fix{};
  if (positions.canAcceptFix() && gnss_manager.takeFreshFixForTransmission(&fix)) {
    if (!positions.acceptFix(fix, orun_tlp::monotonic::nowMs()))
      Serial.println(F("STORAGE position dropped; no live TX"));
  }
  radio_manager.update(!positions.pending());
  orun_tlp::PowerManager::idle();
}
