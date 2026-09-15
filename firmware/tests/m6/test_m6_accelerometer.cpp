#include <assert.h>
#include <initializer_list>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "accelerometer_config.h"
#include "accelerometer_manager.h"
#include "runtime_config.h"
#include <Arduino.h>
#include <Wire.h>

using namespace orun_tlp;

namespace {

void resetHarness() {
  Serial.output.clear();
  fake_wire_timeout_flag = false;
  fake_wire_reset_required_flag = false;
  fake_accelerometer_present = false;
  fake_accelerometer_short_axis_read = false;
  fake_accelerometer_fail_write_register = -1;
  memset(fake_accelerometer_registers, 0,
         sizeof(fake_accelerometer_registers));
  memset(fake_accelerometer_write_regs, 0,
         sizeof(fake_accelerometer_write_regs));
  memset(fake_accelerometer_write_values, 0,
         sizeof(fake_accelerometer_write_values));
  fake_accelerometer_write_count = 0;
  Wire = TwoWire{};

  fake_scl_stuck_low = false;
  fake_sda_stuck_low = false;
  fake_sda_release_after_clocks = -1;
  fake_scl_clock_pulses = 0;
  fake_delay_us = 0;
  memset(pin_modes, 0, sizeof(pin_modes));
  memset(pin_levels, 0, sizeof(pin_levels));
}

void setRawAxis(uint8_t low_register, int16_t mg) {
  const int32_t scaled = static_cast<int32_t>(mg) * 16;
  assert(scaled >= INT16_MIN && scaled <= INT16_MAX);
  const uint16_t raw = static_cast<uint16_t>(static_cast<int16_t>(scaled));
  fake_accelerometer_registers[low_register] =
      static_cast<uint8_t>(raw & 0xFFu);
  fake_accelerometer_registers[static_cast<uint8_t>(low_register + 1)] =
      static_cast<uint8_t>(raw >> 8);
}

void makePresentSensor() {
  fake_accelerometer_present = true;
  fake_accelerometer_registers[0x0F] =
      accelerometer_config::kWhoAmIValue;
}

bool sawWrite(uint8_t reg, uint8_t value) {
  for (unsigned i = 0; i < fake_accelerometer_write_count; ++i) {
    if (fake_accelerometer_write_regs[i] == reg &&
        fake_accelerometer_write_values[i] == value)
      return true;
  }
  return false;
}

void absentDetectionIsBounded() {
  resetHarness();
  AccelerometerManager manager;
  manager.begin(0);

  assert(manager.poll(0) == AccelerometerManager::Event::kNone);
  assert(manager.diagnostics().detection_attempts == 1);
  assert(manager.diagnostics().detection_retries == 1);
  assert(manager.poll(accelerometer_config::kDetectionRetryBackoffMs - 1) ==
         AccelerometerManager::Event::kNone);
  assert(manager.diagnostics().detection_attempts == 1);

  assert(manager.poll(accelerometer_config::kDetectionRetryBackoffMs) ==
         AccelerometerManager::Event::kNone);
  assert(manager.diagnostics().detection_attempts == 2);
  const uint32_t third = accelerometer_config::kDetectionRetryBackoffMs * 2;
  assert(manager.poll(third) == AccelerometerManager::Event::kAbsent);
  assert(manager.detectionComplete());
  assert(!manager.detected());
  assert(!manager.faulted());
  assert(manager.diagnostics().detection_attempts ==
         accelerometer_config::kDetectionMaxAttempts);
  assert(manager.diagnostics().detection_retries == 2);
}

void wrongIdentityIsAbsent() {
  resetHarness();
  fake_accelerometer_present = true;
  fake_accelerometer_registers[0x0F] = 0x42;

  AccelerometerManager manager;
  manager.begin(0);
  assert(manager.poll(0) == AccelerometerManager::Event::kNone);
  assert(manager.poll(250) == AccelerometerManager::Event::kNone);
  assert(manager.poll(500) == AccelerometerManager::Event::kAbsent);
  assert(!manager.detected() && !manager.faulted());
  assert(manager.diagnostics().identity_mismatches == 3);
}

void unresolvedTransportTimeoutDoesNotBecomeAbsent() {
  resetHarness();
  makePresentSensor();

  AccelerometerManager manager;
  manager.begin(0);
  for (uint32_t now : {0U, 250U, 500U}) {
    fake_wire_timeout_flag = true;
    const auto event = manager.poll(now);
    if (now != 500U)
      assert(event == AccelerometerManager::Event::kNone);
    else
      assert(event == AccelerometerManager::Event::kFault);
  }
  assert(manager.detectionComplete());
  assert(!manager.detected());
  assert(manager.faulted());
  assert(manager.diagnostics().i2c_timeouts == 3);
  assert(manager.diagnostics().i2c_recoveries == 3);
}

void successfulProbeUsesFixedUnitsAndPowersDown() {
  resetHarness();
  makePresentSensor();
  fake_accelerometer_registers[0x27] = 0x08;  // ZYXDA.
  setRawAxis(0x28, 100);
  setRawAxis(0x2A, -250);
  setRawAxis(0x2C, 1000);

  AccelerometerManager manager;
  manager.begin(10);
  assert(manager.poll(10) == AccelerometerManager::Event::kNone);
  assert(manager.detected());
  assert(!manager.detectionComplete());
  assert(sawWrite(0x23, 0x88));  // BDU + high resolution, +/-2g.
  assert(sawWrite(0x20, 0x27));  // 10 Hz, XYZ enabled, not low-power mode.

  assert(manager.poll(109) == AccelerometerManager::Event::kNone);
  assert(manager.poll(110) == AccelerometerManager::Event::kPresent);
  assert(manager.detectionComplete());
  assert(manager.detected());
  assert(!manager.faulted());
  assert(manager.diagnostics().probe_samples == 1);
  assert(fake_accelerometer_registers[0x20] == 0x00);  // Probe powers down.

  AccelerometerSample sample{};
  assert(manager.takeProbeSample(&sample));
  assert(sample.captured_at_ms == 110);
  assert(sample.x_mg == 100);
  assert(sample.y_mg == -250);
  assert(sample.z_mg == 1000);
  assert(!manager.takeProbeSample(&sample));
}

void presentDeviceFaultDoesNotDisappear() {
  resetHarness();
  makePresentSensor();
  fake_accelerometer_registers[0x27] = 0x08;
  fake_accelerometer_short_axis_read = true;

  AccelerometerManager manager;
  manager.begin(0);
  assert(manager.poll(0) == AccelerometerManager::Event::kNone);
  assert(manager.poll(100) == AccelerometerManager::Event::kFault);
  assert(manager.detectionComplete());
  assert(manager.detected());
  assert(manager.faulted());
  assert(manager.diagnostics().sample_failures == 1);
  assert(fake_accelerometer_registers[0x20] == 0x00);
}

void recoveredSampleTimeoutIsPresentFault() {
  resetHarness();
  makePresentSensor();
  fake_accelerometer_registers[0x27] = 0x08;

  AccelerometerManager manager;
  manager.begin(0);
  assert(manager.poll(0) == AccelerometerManager::Event::kNone);
  fake_wire_timeout_flag = true;
  assert(manager.poll(100) == AccelerometerManager::Event::kFault);
  assert(manager.detected());
  assert(manager.faulted());
  assert(manager.diagnostics().i2c_timeouts == 1);
  assert(manager.diagnostics().i2c_recoveries == 1);
}

void retryDeadlineIsRolloverSafe() {
  resetHarness();
  const uint32_t start = UINT32_MAX - 100U;
  AccelerometerManager manager;
  manager.begin(start);
  assert(manager.poll(start) == AccelerometerManager::Event::kNone);
  assert(manager.diagnostics().detection_attempts == 1);

  // start + 250 wraps to 149. 100 is still before the deadline.
  assert(manager.poll(100) == AccelerometerManager::Event::kNone);
  assert(manager.diagnostics().detection_attempts == 1);
  assert(manager.poll(149) == AccelerometerManager::Event::kNone);
  assert(manager.diagnostics().detection_attempts == 2);
}

void capabilityCanRepresentPresentFaultSeparately() {
  const CapabilitySnapshot snapshot(
      CapabilityState(true, CapabilityPresence::kPresent,
                      CapabilityHealth::kOk),
      CapabilityState(true, CapabilityPresence::kPresent,
                      CapabilityHealth::kFault));
  assert(snapshot.gnss.presence == CapabilityPresence::kPresent);
  assert(snapshot.accelerometer.supported);
  assert(snapshot.accelerometer.presence == CapabilityPresence::kPresent);
  assert(snapshot.accelerometer.health == CapabilityHealth::kFault);
}

}  // namespace

int main() {
  absentDetectionIsBounded();
  wrongIdentityIsAbsent();
  unresolvedTransportTimeoutDoesNotBecomeAbsent();
  successfulProbeUsesFixedUnitsAndPowersDown();
  presentDeviceFaultDoesNotDisappear();
  recoveredSampleTimeoutIsPresentFault();
  retryDeadlineIsRolloverSafe();
  capabilityCanRepresentPresentFaultSeparately();
  puts("M6A bounded RAK1904 detection/sample checks: PASS");
}
