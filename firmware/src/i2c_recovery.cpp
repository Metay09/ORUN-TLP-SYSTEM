#include "i2c_recovery.h"

#include <Arduino.h>
#include <Wire.h>

extern "C" bool orunWireTakeTimeoutFlag(void);

namespace orun_tlp {
namespace {

constexpr uint32_t kClockPulseUs = 5;
constexpr uint32_t kLineRiseTimeoutUs = 100;
constexpr uint8_t kRecoveryClockPulses = 9;
constexpr uint32_t kI2cClockHz = 100000;

void releaseLine(uint8_t pin) { pinMode(pin, INPUT_PULLUP); }

void pullLineLow(uint8_t pin) {
  digitalWrite(pin, LOW);
  pinMode(pin, OUTPUT);
}

bool waitLineHigh(uint8_t pin) {
  for (uint32_t elapsed = 0; elapsed < kLineRiseTimeoutUs; ++elapsed) {
    if (digitalRead(pin) == HIGH) return true;
    delayMicroseconds(1);
  }
  return digitalRead(pin) == HIGH;
}

bool restartWire(bool bus_free) {
  releaseLine(PIN_WIRE_SDA);
  releaseLine(PIN_WIRE_SCL);
  Wire.begin();
  Wire.setClock(kI2cClockHz);
  return bus_free;
}

}  // namespace

I2cRecoveryResult I2cRecovery::serviceTimeout() {
  if (!orunWireTakeTimeoutFlag()) return I2cRecoveryResult::kNoTimeout;
  return recoverBus() ? I2cRecoveryResult::kRecovered
                      : I2cRecoveryResult::kFailed;
}

bool I2cRecovery::recoverBus() {
  // The patched core has already aborted and re-enabled TWIM. Detach it before
  // bit-banging so only GPIO owns SDA/SCL during recovery.
  Wire.end();
  releaseLine(PIN_WIRE_SDA);
  releaseLine(PIN_WIRE_SCL);
  delayMicroseconds(kClockPulseUs);

  if (!waitLineHigh(PIN_WIRE_SCL)) return restartWire(false);

  // A target holding SDA low may be waiting for clocks to finish a byte. Pulse
  // SCL at most nine times; every wait is bounded so recovery cannot hang too.
  for (uint8_t pulse = 0;
       pulse < kRecoveryClockPulses && digitalRead(PIN_WIRE_SDA) == LOW;
       ++pulse) {
    pullLineLow(PIN_WIRE_SCL);
    delayMicroseconds(kClockPulseUs);
    releaseLine(PIN_WIRE_SCL);
    if (!waitLineHigh(PIN_WIRE_SCL)) return restartWire(false);
    delayMicroseconds(kClockPulseUs);
  }

  if (digitalRead(PIN_WIRE_SDA) == LOW) return restartWire(false);

  // Generate a STOP condition (SDA rising while SCL is high) before restoring
  // TWIM ownership.
  pullLineLow(PIN_WIRE_SDA);
  delayMicroseconds(kClockPulseUs);
  releaseLine(PIN_WIRE_SCL);
  if (!waitLineHigh(PIN_WIRE_SCL)) return restartWire(false);
  delayMicroseconds(kClockPulseUs);
  releaseLine(PIN_WIRE_SDA);
  delayMicroseconds(kClockPulseUs);

  const bool bus_free = digitalRead(PIN_WIRE_SCL) == HIGH &&
                        digitalRead(PIN_WIRE_SDA) == HIGH;
  return restartWire(bus_free);
}

}  // namespace orun_tlp
