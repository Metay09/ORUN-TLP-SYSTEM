#include "accelerometer_manager.h"

#include <Arduino.h>
#include <Wire.h>

#include "accelerometer_config.h"
#include "i2c_recovery.h"
#include "monotonic_time.h"

namespace orun_tlp {
namespace {

constexpr uint8_t kWhoAmI = 0x0F;
constexpr uint8_t kTempCfg = 0x1F;
constexpr uint8_t kCtrl1 = 0x20;
constexpr uint8_t kCtrl2 = 0x21;
constexpr uint8_t kCtrl3 = 0x22;
constexpr uint8_t kCtrl4 = 0x23;
constexpr uint8_t kCtrl5 = 0x24;
constexpr uint8_t kCtrl6 = 0x25;
constexpr uint8_t kStatus = 0x27;
constexpr uint8_t kOutXL = 0x28;
constexpr uint8_t kFifoCtrl = 0x2E;
constexpr uint8_t kInt1Cfg = 0x30;
constexpr uint8_t kClickCfg = 0x38;

constexpr uint8_t kStatusXyzDataAvailable = 0x08;
constexpr uint8_t kCtrl1TenHzXyz = 0x27;   // 10 Hz, normal/HR path, XYZ enabled.
constexpr uint8_t kCtrl4BduHighResolution2g = 0x88;  // BDU + HR, +/-2 g.

struct RegisterWrite {
  uint8_t reg;
  uint8_t value;
};

// CTRL1=0 is deliberately first. RAK1904 VDD remains powered across MCU resets,
// so a reboot/DFU must not assume the LIS3DH already sits in our previous state.
// Configure the remaining registers while powered down and write the requested
// ODR last. The first data-ready sample is discarded separately below because
// LIS3DH output registers retain their last sample in power-down mode.
const RegisterWrite kProbeRegisterWrites[] = {
    {kCtrl1, 0x00},
    {kTempCfg, 0x00},
    {kCtrl2, 0x00},
    {kCtrl3, 0x00},
    {kCtrl5, 0x00},
    {kCtrl6, 0x00},
    {kFifoCtrl, 0x00},
    {kInt1Cfg, 0x00},
    {kClickCfg, 0x00},
    {kCtrl4, kCtrl4BduHighResolution2g},
    {kCtrl1, kCtrl1TenHzXyz},
};
constexpr size_t kProbeRegisterWriteCount =
    sizeof(kProbeRegisterWrites) / sizeof(kProbeRegisterWrites[0]);
static_assert(kProbeRegisterWriteCount <= UINT8_MAX,
              "M6A configuration step index must fit uint8_t");

// LIS3DH high-resolution +/-2 g output is signed 12-bit, left-justified in the
// 16-bit OUT registers. Sensitivity is 1 mg/digit, so divide the assembled
// signed value by 16. Valid high-resolution samples have zero low four bits.
int16_t highResolution2gToMg(uint8_t low, uint8_t high) {
  const uint16_t assembled = static_cast<uint16_t>(low) |
                             (static_cast<uint16_t>(high) << 8);
  const int16_t raw = static_cast<int16_t>(assembled);
  return static_cast<int16_t>(raw / 16);
}

enum class BusResult : uint8_t {
  kOk,
  kIoError,
  kTimeoutRecovered,
  kTimeoutFailed,
};

BusResult finishBusOperation(bool io_ok) {
  const I2cRecoveryResult recovery = I2cRecovery::serviceTimeout();
  if (recovery == I2cRecoveryResult::kRecovered)
    return BusResult::kTimeoutRecovered;
  if (recovery == I2cRecoveryResult::kFailed)
    return BusResult::kTimeoutFailed;
  return io_ok ? BusResult::kOk : BusResult::kIoError;
}

BusResult readRegister(uint8_t reg, uint8_t* value) {
  if (value == nullptr) return BusResult::kIoError;

  Wire.beginTransmission(accelerometer_config::kI2cAddress);
  const bool wrote_register = Wire.write(reg) == 1;
  const uint8_t tx_status = Wire.endTransmission(false);
  if (!wrote_register || tx_status != 0)
    return finishBusOperation(false);

  const uint8_t returned = Wire.requestFrom(
      static_cast<uint8_t>(accelerometer_config::kI2cAddress),
      static_cast<uint8_t>(1));
  const BusResult bus = finishBusOperation(returned == 1);
  if (bus != BusResult::kOk) return bus;

  const int byte = Wire.read();
  if (byte < 0) return BusResult::kIoError;
  *value = static_cast<uint8_t>(byte);
  return BusResult::kOk;
}

BusResult writeRegister(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(accelerometer_config::kI2cAddress);
  const bool wrote_register = Wire.write(reg) == 1;
  const bool wrote_value = Wire.write(value) == 1;
  const uint8_t tx_status = Wire.endTransmission(true);
  return finishBusOperation(wrote_register && wrote_value && tx_status == 0);
}

BusResult readAxes(uint8_t* bytes, uint8_t length) {
  if (bytes == nullptr || length != 6) return BusResult::kIoError;

  Wire.beginTransmission(accelerometer_config::kI2cAddress);
  // LIS3DH I2C multi-byte reads require bit 7 of the sub-address.
  const bool wrote_register = Wire.write(static_cast<uint8_t>(kOutXL | 0x80)) == 1;
  const uint8_t tx_status = Wire.endTransmission(false);
  if (!wrote_register || tx_status != 0)
    return finishBusOperation(false);

  const uint8_t returned = Wire.requestFrom(
      static_cast<uint8_t>(accelerometer_config::kI2cAddress), length);
  const BusResult bus = finishBusOperation(returned == length);
  if (bus != BusResult::kOk) return bus;

  for (uint8_t i = 0; i < length; ++i) {
    const int byte = Wire.read();
    if (byte < 0) return BusResult::kIoError;
    bytes[i] = static_cast<uint8_t>(byte);
  }
  return BusResult::kOk;
}

void accountBusResult(BusResult result,
                      AccelerometerManager::Diagnostics& diagnostics,
                      bool& saw_transport_timeout) {
  if (result == BusResult::kTimeoutRecovered ||
      result == BusResult::kTimeoutFailed) {
    ++diagnostics.i2c_timeouts;
    saw_transport_timeout = true;
  }
  if (result == BusResult::kTimeoutRecovered)
    ++diagnostics.i2c_recoveries;
  else if (result == BusResult::kTimeoutFailed)
    ++diagnostics.i2c_recovery_failures;
}

bool powerDownSensor(AccelerometerManager::Diagnostics& diagnostics,
                     bool& saw_transport_timeout) {
  const BusResult result = writeRegister(kCtrl1, 0x00);
  accountBusResult(result, diagnostics, saw_transport_timeout);
  if (result == BusResult::kOk) return true;
  ++diagnostics.power_down_failures;
  return false;
}

}  // namespace

void AccelerometerManager::begin(uint32_t now) {
  *this = AccelerometerManager{};
  Wire.begin();
  state_ = State::kDetecting;
  next_action_at_ms_ = now;
}

void AccelerometerManager::scheduleDetectionRetry(uint32_t now) {
  state_ = State::kDetectionBackoff;
  next_action_at_ms_ = now + accelerometer_config::kDetectionRetryBackoffMs;
  ++diagnostics_.detection_retries;
}

AccelerometerManager::Event AccelerometerManager::finishAbsent() {
  detection_complete_ = true;
  detected_ = false;
  faulted_ = false;
  probe_sample_ready_ = false;
  pending_finish_event_ = Event::kNone;
  state_ = State::kDone;
  return Event::kAbsent;
}

AccelerometerManager::Event AccelerometerManager::finishFault() {
  detection_complete_ = true;
  faulted_ = true;
  probe_sample_ready_ = false;
  pending_finish_event_ = Event::kNone;
  state_ = State::kDone;
  return Event::kFault;
}

AccelerometerManager::Event AccelerometerManager::finishPresent() {
  detection_complete_ = true;
  detected_ = true;
  faulted_ = false;
  probe_sample_ready_ = true;
  pending_finish_event_ = Event::kNone;
  state_ = State::kDone;
  return Event::kPresent;
}

AccelerometerManager::Event AccelerometerManager::poll(uint32_t now) {
  if (state_ == State::kDone) return Event::kNone;

  if (state_ == State::kDetectionBackoff) {
    if (!monotonic::reached(now, next_action_at_ms_)) return Event::kNone;
    state_ = State::kDetecting;
  }

  if (state_ == State::kDetecting) {
    if (!monotonic::reached(now, next_action_at_ms_)) return Event::kNone;

    ++detection_attempts_;
    ++diagnostics_.detection_attempts;
    uint8_t who_am_i = 0;
    const BusResult result = readRegister(kWhoAmI, &who_am_i);
    accountBusResult(result, diagnostics_, saw_transport_timeout_);

    if (result == BusResult::kTimeoutFailed)
      return finishFault();

    if (result != BusResult::kOk) {
      if (detection_attempts_ >= accelerometer_config::kDetectionMaxAttempts)
        return saw_transport_timeout_ ? finishFault() : finishAbsent();
      scheduleDetectionRetry(now);
      return Event::kNone;
    }

    if (who_am_i != accelerometer_config::kWhoAmIValue) {
      ++diagnostics_.identity_mismatches;
      if (detection_attempts_ >= accelerometer_config::kDetectionMaxAttempts)
        return finishAbsent();
      scheduleDetectionRetry(now);
      return Event::kNone;
    }

    // Positive WHO_AM_I proves the accelerometer is physically present. From
    // this point on, a transport/config/sample failure is health failure, not
    // evidence that the installed device disappeared. Configuration is serviced
    // one register per cooperative pass so M6A does not monopolize the shared
    // I2C/owner loop behind a sequence of individually bounded Wire waits.
    detected_ = true;
    discard_next_sample_ = true;
    configuration_step_ = 0;
    state_ = State::kConfiguring;
    return Event::kNone;
  }

  if (state_ == State::kConfiguring) {
    const RegisterWrite& write = kProbeRegisterWrites[configuration_step_];
    const BusResult result = writeRegister(write.reg, write.value);
    accountBusResult(result, diagnostics_, saw_transport_timeout_);
    if (result != BusResult::kOk) {
      ++diagnostics_.configuration_failures;
      pending_finish_event_ = Event::kFault;
      state_ = State::kPoweringDown;
      return Event::kNone;
    }

    ++configuration_step_;
    if (configuration_step_ == kProbeRegisterWriteCount) {
      probe_started_at_ms_ = now;
      next_action_at_ms_ = now + accelerometer_config::kProbeSamplePeriodMs;
      state_ = State::kProbeWait;
    }
    return Event::kNone;
  }

  if (state_ == State::kProbeWait) {
    if (!monotonic::reached(now, next_action_at_ms_)) return Event::kNone;

    uint8_t status = 0;
    const BusResult result = readRegister(kStatus, &status);
    accountBusResult(result, diagnostics_, saw_transport_timeout_);
    if (result != BusResult::kOk) {
      ++diagnostics_.sample_failures;
      pending_finish_event_ = Event::kFault;
      state_ = State::kPoweringDown;
      return Event::kNone;
    }

    if ((status & kStatusXyzDataAvailable) == 0) {
      if (monotonic::elapsed(now, probe_started_at_ms_,
                             accelerometer_config::kProbeTimeoutMs)) {
        ++diagnostics_.sample_failures;
        pending_finish_event_ = Event::kFault;
        state_ = State::kPoweringDown;
        return Event::kNone;
      }
      next_action_at_ms_ = now + accelerometer_config::kProbeSamplePeriodMs;
      return Event::kNone;
    }

    state_ = State::kReadingSample;
    return Event::kNone;
  }

  if (state_ == State::kReadingSample) {
    uint8_t bytes[6]{};
    const BusResult result = readAxes(bytes, sizeof(bytes));
    accountBusResult(result, diagnostics_, saw_transport_timeout_);
    if (result != BusResult::kOk) {
      ++diagnostics_.sample_failures;
      pending_finish_event_ = Event::kFault;
      state_ = State::kPoweringDown;
      return Event::kNone;
    }

    if (discard_next_sample_) {
      // LIS3DH retains the previous output registers in power-down mode. Reading
      // and discarding the first ready set prevents a pre-reset/pre-DFU sample
      // from being reported as a fresh M6A observation. Wait for the next ODR
      // period before accepting a sample from the newly configured session.
      discard_next_sample_ = false;
      next_action_at_ms_ = now + accelerometer_config::kProbeSamplePeriodMs;
      state_ = State::kProbeWait;
      return Event::kNone;
    }

    probe_sample_ = AccelerometerSample(
        now, highResolution2gToMg(bytes[0], bytes[1]),
        highResolution2gToMg(bytes[2], bytes[3]),
        highResolution2gToMg(bytes[4], bytes[5]));
    ++diagnostics_.probe_samples;
    pending_finish_event_ = Event::kPresent;
    state_ = State::kPoweringDown;
    return Event::kNone;
  }

  if (state_ == State::kPoweringDown) {
    if (!powerDownSensor(diagnostics_, saw_transport_timeout_))
      return finishFault();
    if (pending_finish_event_ == Event::kPresent)
      return finishPresent();
    return finishFault();
  }

  return Event::kNone;
}

bool AccelerometerManager::takeProbeSample(AccelerometerSample* sample) {
  if (sample == nullptr || !probe_sample_ready_) return false;
  *sample = probe_sample_;
  probe_sample_ready_ = false;
  return true;
}

}  // namespace orun_tlp
