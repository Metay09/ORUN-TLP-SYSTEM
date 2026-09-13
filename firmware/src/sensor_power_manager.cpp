#include "sensor_power_manager.h"

#include <Arduino.h>

namespace orun_tlp {

uint32_t SensorPowerManager::owners_ = 0;

namespace {
uint32_t ownerBit(SensorPowerOwner owner) {
  return 1UL << static_cast<uint8_t>(owner);
}
}  // namespace

void SensorPowerManager::begin() {
  owners_ = 0;
  pinMode(WB_IO2, OUTPUT);
  digitalWrite(WB_IO2, LOW);
}

void SensorPowerManager::acquire(SensorPowerOwner owner) {
  const uint32_t before = owners_;
  owners_ |= ownerBit(owner);
  if (before == 0 && owners_ != 0) digitalWrite(WB_IO2, HIGH);
}

void SensorPowerManager::release(SensorPowerOwner owner) {
  const uint32_t before = owners_;
  owners_ &= ~ownerBit(owner);
  if (before != 0 && owners_ == 0) digitalWrite(WB_IO2, LOW);
}

bool SensorPowerManager::powered() { return owners_ != 0; }

uint32_t SensorPowerManager::ownerMask() { return owners_; }

}  // namespace orun_tlp
