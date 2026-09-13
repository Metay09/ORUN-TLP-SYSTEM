#include <SparkFun_u-blox_GNSS_Arduino_Library.h>
#define private public
#include "gnss_manager.h"
#undef private

#include "../m3/gnss_test_support.h"
#include "i2c_recovery.h"
#include "sensor_power_manager.h"
#include "watchdog_manager.h"

void resetBusFixture() {
  fake_wire_timeout_flag = false;
  fake_wire_reset_required_flag = false;
  fake_scl_stuck_low = false;
  fake_sda_stuck_low = false;
  fake_sda_release_after_clocks = -1;
  fake_scl_clock_pulses = 0;
  fake_delay_us = 0;
  Wire.begin_calls = Wire.end_calls = Wire.set_clock_calls = 0;
  Wire.last_clock_hz = 0;
  pin_modes[PIN_WIRE_SDA] = pin_modes[PIN_WIRE_SCL] = INPUT_PULLUP;
  pin_levels[PIN_WIRE_SDA] = pin_levels[PIN_WIRE_SCL] = HIGH;
}

void powerOwnershipIsCentralized() {
  SensorPowerManager::begin();
  assert(!SensorPowerManager::powered() && sensor_power == LOW);
  SensorPowerManager::acquire(SensorPowerOwner::kGnss);
  assert(SensorPowerManager::powered() && sensor_power == HIGH);
  const auto first_mask = SensorPowerManager::ownerMask();
  SensorPowerManager::acquire(SensorPowerOwner::kGnss); // idempotent
  assert(SensorPowerManager::ownerMask() == first_mask && sensor_power == HIGH);
  SensorPowerManager::acquire(SensorPowerOwner::kAuxiliary);
  SensorPowerManager::release(SensorPowerOwner::kGnss);
  assert(SensorPowerManager::powered() && sensor_power == HIGH);
  SensorPowerManager::release(SensorPowerOwner::kAuxiliary);
  assert(!SensorPowerManager::powered() && sensor_power == LOW);
}

void boundedRecoveryPrimitive() {
  resetBusFixture();
  assert(I2cRecovery::serviceTimeout() == I2cRecoveryResult::kNoTimeout);

  fake_wire_timeout_flag = true;
  assert(I2cRecovery::serviceTimeout() == I2cRecoveryResult::kRecovered);
  assert(Wire.end_calls == 1 && Wire.begin_calls == 1);
  assert(Wire.set_clock_calls == 1 && Wire.last_clock_hz == 100000);
  assert(!fake_wire_timeout_flag);

  resetBusFixture();
  fake_wire_timeout_flag = true;
  fake_sda_stuck_low = true;
  fake_sda_release_after_clocks = 3;
  assert(I2cRecovery::serviceTimeout() == I2cRecoveryResult::kRecovered);
  assert(fake_scl_clock_pulses >= 3);

  resetBusFixture();
  fake_wire_timeout_flag = true;
  fake_sda_stuck_low = true;
  assert(I2cRecovery::serviceTimeout() == I2cRecoveryResult::kFailed);
  assert(fake_scl_clock_pulses == 9);
  assert(Wire.end_calls == 1 && Wire.begin_calls == 1);

  resetBusFixture();
  fake_wire_timeout_flag = true;
  fake_scl_stuck_low = true;
  assert(I2cRecovery::serviceTimeout() == I2cRecoveryResult::kFailed);
  assert(Wire.end_calls == 1 && Wire.begin_calls == 1);
}

void unsafeTwimAbortNeverTouchesGpioRecovery() {
  resetBusFixture();
  fake_wire_timeout_flag = true;
  fake_wire_reset_required_flag = true;
  assert(I2cRecovery::serviceTimeout() == I2cRecoveryResult::kFailed);
  // Host builds cannot reset the MCU, but they prove the unsafe-abort handoff
  // does not call Wire.end(), clock GPIO, or restart the peripheral.
  assert(Wire.end_calls == 0 && Wire.begin_calls == 0);
  assert(fake_scl_clock_pulses == 0);
  assert(!fake_wire_reset_required_flag);
}

void callbackGetterTimeoutFailsClosedImmediately() {
  GnssManager manager;
  boot(manager);
  resetBusFixture();

  auto value = pvt(2000);
  Fake::current_pvt = value;
  Fake::itow_fresh = false;  // Force the defensive cache-miss path in the stub.
  Fake::time_of_week_timeout_on_cache_miss = true;
  const uint32_t generation = manager.session_generation_;

  manager.handlePvt(value);

  Fake::time_of_week_timeout_on_cache_miss = false;
  assert(Fake::time_of_week_cache_misses == 1);
  assert(manager.state() == State::kStarting);
  assert(manager.session_generation_ != generation);
  assert(manager.diagnostics().i2c_timeouts == 1);
  assert(manager.diagnostics().i2c_recoveries == 1);
  assert(!manager.has_candidate_fix_ && !manager.has_latest_hdop_);
  GnssFix fix{};
  assert(!manager.takeFreshFixForTransmission(&fix));
}

void gnssTimeoutRestartsBehindFreshnessBoundary() {
  GnssManager manager;
  boot(manager);
  emitPvt(manager, pvt(1000)); emitDop(manager, 1000); // boundaries
  const uint32_t generation = manager.session_generation_;

  fake_wire_timeout_flag = true;
  manager.poll();
  assert(manager.state() == State::kStarting);
  assert(manager.session_generation_ != generation);
  assert(manager.diagnostics().i2c_timeouts == 1);
  assert(manager.diagnostics().i2c_recoveries == 1);
  assert(!manager.has_candidate_fix_ && !manager.has_latest_hdop_);
  prepare(manager);

  GnssFix fix{};
  emitPvt(manager, pvt(2000)); emitDop(manager, 2000);
  assert(!manager.takeFreshFixForTransmission(&fix));
  emitPvt(manager, pvt(3000)); emitDop(manager, 3000);
  assert(manager.takeFreshFixForTransmission(&fix));
}

void recoveryFailureAndBudgetFailClosed() {
  {
    GnssManager manager;
    boot(manager);
    fake_wire_timeout_flag = true;
    fake_scl_stuck_low = true;
    manager.poll();
    assert(manager.state() == State::kFailure);
    assert(manager.diagnostics().i2c_timeouts == 1);
    assert(manager.diagnostics().i2c_recovery_failures == 1);
    fake_scl_stuck_low = false;
    manager.poll();
    assert(manager.state() == State::kSleeping);
    assert(sensor_power == LOW);
  }

  {
    GnssManager manager;
    boot(manager);
    for (uint8_t n = 0; n < gnss_config::kMaxI2cRecoveriesPerAcquisition; ++n) {
      fake_wire_timeout_flag = true;
      manager.poll();
      assert(manager.state() == State::kStarting);
      prepare(manager);
    }
    fake_wire_timeout_flag = true;
    manager.poll();
    assert(manager.state() == State::kFailure);
    assert(manager.diagnostics().i2c_recovery_failures == 1);
    assert(manager.diagnostics().i2c_timeouts ==
           gnss_config::kMaxI2cRecoveriesPerAcquisition + 1);
  }
}

void watchdogApiIsStable() {
  static_assert(WatchdogManager::kTimeoutSeconds == 30,
                "R4 watchdog timeout policy changed");
  WatchdogManager::begin();
  WatchdogManager::feed();
  assert(!WatchdogManager::bootInfo().watchdog_reset); // host build has no NRF WDT
}

int main() {
  powerOwnershipIsCentralized();
  boundedRecoveryPrimitive();
  unsafeTwimAbortNeverTouchesGpioRecovery();
  callbackGetterTimeoutFailsClosedImmediately();
  gnssTimeoutRestartsBehindFreshnessBoundary();
  recoveryFailureAndBudgetFailClosed();
  watchdogApiIsStable();
  puts("R4 bounded I2C recovery, callback fail-closed, power ownership and watchdog API checks: PASS");
}
