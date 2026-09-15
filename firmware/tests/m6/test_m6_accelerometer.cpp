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

constexpr unsigned kProbeConfigurationWrites = 11;

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

uint32_t runConfigurationOneWritePerPoll(AccelerometerManager& manager,
                                         uint32_t first_poll_ms) {
  uint32_t now = first_poll_ms;
  for (unsigned i = 0; i < kProbeConfigurationWrites; ++i, ++now) {
    const unsigned writes_before = fake_accelerometer_write_count;
    assert(manager.poll(now) == AccelerometerManager::Event::kNone);
    assert(fake_accelerometer_write_count == writes_before + 1);
  }
  return now - 1;
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

void successfulProbeDiscardsRetainedSampleAndPowersDown() {
  resetHarness();
  makePresentSensor();
  fake_accelerometer_registers[0x27] = 0x08;  // ZYXDA.

  // Model values retained from a previous MCU session. LIS3DH keeps output
  // registers in power-down, so M6A must never publish these as a fresh sample.
  setRawAxis(0x28, 100);
  setRawAxis(0x2A, -250);
  setRawAxis(0x2C, 1000);

  AccelerometerManager manager;
  manager.begin(10);
  assert(manager.poll(10) == AccelerometerManager::Event::kNone);
  assert(manager.detected());
  assert(!manager.detectionComplete());
  assert(fake_accelerometer_write_count == 0);

  const uint32_t configured_at = runConfigurationOneWritePerPoll(manager, 11);
  assert(configured_at == 21);
  assert(fake_accelerometer_write_count == kProbeConfigurationWrites);
  assert(fake_accelerometer_write_regs[0] == 0x20);
  assert(fake_accelerometer_write_values[0] == 0x00);  // Power down first.
  assert(fake_accelerometer_write_regs[kProbeConfigurationWrites - 1] == 0x20);
  assert(fake_accelerometer_write_values[kProbeConfigurationWrites - 1] == 0x27);
  assert(sawWrite(0x23, 0x88));  // BDU + high resolution, +/-2g.

  const uint32_t first_sample_due =
      configured_at + accelerometer_config::kProbeSamplePeriodMs;
  assert(manager.poll(first_sample_due - 1) ==
         AccelerometerManager::Event::kNone);
  assert(manager.poll(first_sample_due) == AccelerometerManager::Event::kNone);
  // First output read is deliberately discarded as possibly retained/stale.
  assert(manager.poll(first_sample_due + 1) ==
         AccelerometerManager::Event::kNone);

  // The next ODR sample is the first one M6A is allowed to publish.
  setRawAxis(0x28, 200);
  setRawAxis(0x2A, -300);
  setRawAxis(0x2C, 900);
  const uint32_t fresh_sample_due =
      first_sample_due + 1 + accelerometer_config::kProbeSamplePeriodMs;
  assert(manager.poll(fresh_sample_due - 1) ==
         AccelerometerManager::Event::kNone);
  assert(manager.poll(fresh_sample_due) == AccelerometerManager::Event::kNone);
  assert(manager.poll(fresh_sample_due + 1) ==
         AccelerometerManager::Event::kNone);
  // Power-down is its own pass; only then is PRESENT published.
  assert(manager.poll(fresh_sample_due + 2) ==
         AccelerometerManager::Event::kPresent);

  assert(manager.detectionComplete());
  assert(manager.detected());
  assert(!manager.faulted());
  assert(manager.diagnostics().probe_samples == 1);
  assert(fake_accelerometer_registers[0x20] == 0x00);  // Probe powers down.
  assert(fake_accelerometer_write_count == kProbeConfigurationWrites + 1);

  AccelerometerSample sample{};
  assert(manager.takeProbeSample(&sample));
  assert(sample.captured_at_ms == fresh_sample_due + 1);
  assert(sample.x_mg == 200);
  assert(sample.y_mg == -300);
  assert(sample.z_mg == 900);
  assert(!manager.takeProbeSample(&sample));
}

void configurationFailureDefersCleanup() {
  resetHarness();
  makePresentSensor();
  fake_accelerometer_fail_write_register = 0x22;  // CTRL3.

  AccelerometerManager manager;
  manager.begin(0);
  assert(manager.poll(0) == AccelerometerManager::Event::kNone);
  assert(manager.detected());

  assert(manager.poll(1) == AccelerometerManager::Event::kNone);  // CTRL1=0.
  assert(manager.poll(2) == AccelerometerManager::Event::kNone);  // TEMP_CFG.
  assert(manager.poll(3) == AccelerometerManager::Event::kNone);  // CTRL2.
  // The failing CTRL3 write does not perform cleanup in the same cooperative pass.
  const unsigned writes_before_failure = fake_accelerometer_write_count;
  assert(manager.poll(4) == AccelerometerManager::Event::kNone);
  assert(fake_accelerometer_write_count == writes_before_failure);
  assert(manager.diagnostics().configuration_failures == 1);
  assert(!manager.detectionComplete());

  fake_accelerometer_fail_write_register = -1;
  assert(manager.poll(5) == AccelerometerManager::Event::kFault);
  assert(manager.detectionComplete());
  assert(manager.detected());
  assert(manager.faulted());
  assert(fake_accelerometer_registers[0x20] == 0x00);
}

void presentDeviceFaultDoesNotDisappear() {
  resetHarness();
  makePresentSensor();
  fake_accelerometer_registers[0x27] = 0x08;
  fake_accelerometer_short_axis_read = true;

  AccelerometerManager manager;
  manager.begin(0);
  assert(manager.poll(0) == AccelerometerManager::Event::kNone);
  const uint32_t configured_at = runConfigurationOneWritePerPoll(manager, 1);
  const uint32_t sample_due =
      configured_at + accelerometer_config::kProbeSamplePeriodMs;
  assert(manager.poll(sample_due) == AccelerometerManager::Event::kNone);
  assert(manager.poll(sample_due + 1) == AccelerometerManager::Event::kNone);
  assert(manager.poll(sample_due + 2) == AccelerometerManager::Event::kFault);
  assert(manager.detectionComplete());
  assert(manager.detected());
  assert(manager.faulted());
  assert(manager.diagnostics().sample_failures == 1);
  assert(fake_accelerometer_registers[0x20] == 0x00);
  AccelerometerSample sample{};
  assert(!manager.takeProbeSample(&sample));
}

void recoveredSampleTimeoutIsPresentFault() {
  resetHarness();
  makePresentSensor();
  fake_accelerometer_registers[0x27] = 0x08;

  AccelerometerManager manager;
  manager.begin(0);
  assert(manager.poll(0) == AccelerometerManager::Event::kNone);
  const uint32_t configured_at = runConfigurationOneWritePerPoll(manager, 1);
  const uint32_t sample_due =
      configured_at + accelerometer_config::kProbeSamplePeriodMs;
  fake_wire_timeout_flag = true;
  assert(manager.poll(sample_due) == AccelerometerManager::Event::kNone);
  assert(manager.poll(sample_due + 1) == AccelerometerManager::Event::kFault);
  assert(manager.detected());
  assert(manager.faulted());
  assert(manager.diagnostics().i2c_timeouts == 1);
  assert(manager.diagnostics().i2c_recoveries == 1);
}

void finalPowerDownFailureDoesNotPublishSample() {
  resetHarness();
  makePresentSensor();
  fake_accelerometer_registers[0x27] = 0x08;
  setRawAxis(0x28, 10);
  setRawAxis(0x2A, 20);
  setRawAxis(0x2C, 1000);

  AccelerometerManager manager;
  manager.begin(0);
  assert(manager.poll(0) == AccelerometerManager::Event::kNone);
  const uint32_t configured_at = runConfigurationOneWritePerPoll(manager, 1);
  const uint32_t first_due =
      configured_at + accelerometer_config::kProbeSamplePeriodMs;
  assert(manager.poll(first_due) == AccelerometerManager::Event::kNone);
  assert(manager.poll(first_due + 1) == AccelerometerManager::Event::kNone);

  const uint32_t fresh_due =
      first_due + 1 + accelerometer_config::kProbeSamplePeriodMs;
  assert(manager.poll(fresh_due) == AccelerometerManager::Event::kNone);
  assert(manager.poll(fresh_due + 1) == AccelerometerManager::Event::kNone);
  assert(manager.diagnostics().probe_samples == 1);

  fake_accelerometer_fail_write_register = 0x20;
  assert(manager.poll(fresh_due + 2) == AccelerometerManager::Event::kFault);
  assert(manager.detected());
  assert(manager.faulted());
  assert(manager.diagnostics().power_down_failures == 1);
  AccelerometerSample sample{};
  assert(!manager.takeProbeSample(&sample));
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
  successfulProbeDiscardsRetainedSampleAndPowersDown();
  configurationFailureDefersCleanup();
  presentDeviceFaultDoesNotDisappear();
  recoveredSampleTimeoutIsPresentFault();
  finalPowerDownFailureDoesNotPublishSample();
  retryDeadlineIsRolloverSafe();
  capabilityCanRepresentPresentFaultSeparately();
  puts("M6A bounded RAK1904 detection/sample checks: PASS");
}
