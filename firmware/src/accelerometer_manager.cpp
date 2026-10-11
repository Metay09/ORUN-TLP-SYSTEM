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
constexpr uint8_t kFifoSrc = 0x2F;
constexpr uint8_t kInt1Cfg = 0x30;
constexpr uint8_t kClickCfg = 0x38;
constexpr uint8_t kActThs = 0x3E;

constexpr uint8_t kStatusXyzDataAvailable = 0x08;
constexpr uint8_t kCtrl1TenHzXyz = 0x27;   // 10 Hz, normal/HR path, XYZ enabled.
constexpr uint8_t kCtrl4BduHighResolution2g = 0x88;  // BDU + HR, +/-2 g.
constexpr uint8_t kCtrl4BduNormal2g = 0x80;          // BDU, normal, +/-2 g.
constexpr uint8_t kCtrl5FifoEnable = 0x40;
// FIFO_CTRL_REG FM[1:0] in bits 7:6 (AN3308): Bypass empties the FIFO,
// Stream keeps the newest 32 samples and flags an overrun.
constexpr uint8_t kFifoModeBypass = 0x00;
constexpr uint8_t kFifoModeStream = 0x80;
// FIFO_SRC_REG: OVRN_FIFO (32 unread samples, the oldest is being
// overwritten), EMPTY, and FSS[4:0] = unread samples (31 also when full).
constexpr uint8_t kFifoSrcOverrun = 0x40;
constexpr uint8_t kFifoSrcLevelMask = 0x1F;

struct RegisterWrite {
  uint8_t reg;
  uint8_t value;
};

// CTRL1=0 is deliberately first. RAK1904 VDD remains powered across MCU resets,
// so a reboot/DFU must not assume the LIS3DH already sits in our previous state.
// Configure the remaining registers while powered down and write the requested
// ODR last. Clear ACT_THS explicitly because a retained non-zero value enables
// LIS3DH autonomous activity/inactivity mode and can force 10 Hz low-power state.
// FIFO_CTRL itself need not be reset while CTRL5 FIFO_EN is explicitly cleared.
// The first data-ready sample is discarded separately below because LIS3DH output
// registers retain their last sample in power-down mode.
const RegisterWrite kProbeRegisterWrites[] = {
    {kCtrl1, 0x00},
    {kTempCfg, 0x00},
    {kCtrl2, 0x00},
    {kCtrl3, 0x00},
    {kCtrl5, 0x00},
    {kCtrl6, 0x00},
    {kInt1Cfg, 0x00},
    {kClickCfg, 0x00},
    {kActThs, 0x00},
    {kCtrl4, kCtrl4BduHighResolution2g},
    {kCtrl1, kCtrl1TenHzXyz},
};
constexpr size_t kProbeRegisterWriteCount =
    sizeof(kProbeRegisterWrites) / sizeof(kProbeRegisterWrites[0]);
static_assert(kProbeRegisterWriteCount <= UINT8_MAX,
              "M6A configuration step index must fit uint8_t");

// Continuous stream: the same reset-safe order (CTRL1=0 first, ODR last).
// The FIFO is enabled but held in Bypass, which stores nothing; Stream mode
// is selected only after the turn-on settle (kStreamSettling), as AN3308
// requires FIFO_EN before a FIFO mode is chosen.
const RegisterWrite kStreamRegisterWrites[] = {
    {kCtrl1, 0x00},
    {kTempCfg, 0x00},
    {kCtrl2, 0x00},
    {kCtrl3, 0x00},
    {kCtrl6, 0x00},
    {kInt1Cfg, 0x00},
    {kClickCfg, 0x00},
    {kActThs, 0x00},
    {kFifoCtrl, kFifoModeBypass},
    {kCtrl5, kCtrl5FifoEnable},
    {kCtrl4, kCtrl4BduNormal2g},
    {kCtrl1, kCtrl1TenHzXyz},
};
constexpr size_t kStreamRegisterWriteCount =
    sizeof(kStreamRegisterWrites) / sizeof(kStreamRegisterWrites[0]);
static_assert(kStreamRegisterWriteCount <= UINT8_MAX,
              "stream configuration step index must fit uint8_t");

// LIS3DH high-resolution +/-2 g output is signed 12-bit, left-justified in the
// 16-bit OUT registers. Sensitivity is 1 mg/digit, so divide the assembled
// signed value by 16. Valid high-resolution samples have zero low four bits.
int16_t highResolution2gToMg(uint8_t low, uint8_t high) {
  const uint16_t assembled = static_cast<uint16_t>(low) |
                             (static_cast<uint16_t>(high) << 8);
  const int16_t raw = static_cast<int16_t>(assembled);
  return static_cast<int16_t>(raw / 16);
}

// Normal mode +/-2 g (the FIFO stream): signed 10-bit, left-justified,
// 4 mg/digit, so the same left-justified value divided by 16 is milli-g.
int16_t normal2gToMg(uint8_t low, uint8_t high) {
  const uint16_t assembled = static_cast<uint16_t>(low) |
                             (static_cast<uint16_t>(high) << 8);
  const int16_t raw = static_cast<int16_t>(assembled);
  return static_cast<int16_t>((raw / 64) * 4);
}

// A drain is plausible when its sample count is within a quarter (plus two
// for phase) of what 10 Hz gives over the elapsed time. Far outside means the
// sensor is not sampling at the configured rate; an overrun is judged apart.
bool streamCountPlausible(uint32_t stored, uint32_t elapsed_ms) {
  const uint32_t expected = elapsed_ms / accelerometer_config::kProbeSamplePeriodMs;
  const uint32_t slack = expected / 4U + 2U;
  return stored + slack >= expected && stored <= expected + slack;
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

// Reads one or more 6-byte sample sets from OUT_X_L. With the FIFO enabled
// the address rolls back from 0x2D to 0x28, so each set is the next-oldest
// FIFO sample (AN3308).
BusResult readAxes(uint8_t* bytes, uint8_t length) {
  if (bytes == nullptr || length == 0 || length % 6U != 0 ||
      length > accelerometer_config::kStreamMaxSamplesPerRead * 6U)
    return BusResult::kIoError;

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

void AccelerometerManager::startPowerDown(Event event) {
  if (runtime_active_ || stream_active_) {
    runtime_sample_ready_ = false;
    clearStreamBuffer();
    // Fault visibility is immediate; shutdown continues cooperatively.
    if (event == Event::kFault) faulted_ = true;
  }
  pending_finish_event_ = event;
  power_down_attempts_ = 0;
  state_ = State::kPoweringDown;
}

AccelerometerManager::Event AccelerometerManager::enterFaultCleanup(uint32_t now) {
  // The immediate shutdown budget is exhausted, so publish the capability fault
  // now and suppress any captured sample. Do not abandon a positively identified
  // sensor in active mode forever: retain a sparse one-operation cleanup state.
  // The capability stays faulted even if a later cleanup succeeds; a reboot is
  // required to re-probe health rather than silently converting FAULT to OK.
  detection_complete_ = true;
  faulted_ = true;
  runtime_active_ = false;
  runtime_sample_ready_ = false;
  stream_active_ = false;
  clearStreamBuffer();
  probe_sample_ready_ = false;
  pending_finish_event_ = Event::kNone;
  next_action_at_ms_ = now + accelerometer_config::kFaultCleanupRetryBackoffMs;
  state_ = State::kFaultCleanup;
  return Event::kFault;
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
  runtime_active_ = false;
  runtime_sample_ready_ = false;
  stream_active_ = false;
  clearStreamBuffer();
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

  if (state_ == State::kStreamSettling || state_ == State::kStreamWaiting ||
      state_ == State::kStreamReading)
    return pollStream(now);

  if (state_ == State::kConfiguring) {
    const RegisterWrite* writes =
        stream_active_ ? kStreamRegisterWrites : kProbeRegisterWrites;
    const size_t write_count =
        stream_active_ ? kStreamRegisterWriteCount : kProbeRegisterWriteCount;
    const RegisterWrite& write = writes[configuration_step_];
    const BusResult result = writeRegister(write.reg, write.value);
    accountBusResult(result, diagnostics_, saw_transport_timeout_);
    if (result != BusResult::kOk) {
      ++diagnostics_.configuration_failures;
      startPowerDown(Event::kFault);
      return Event::kNone;
    }

    ++configuration_step_;
    if (configuration_step_ == write_count && stream_active_) {
      next_action_at_ms_ = now + accelerometer_config::kStreamSettleMs;
      state_ = State::kStreamSettling;
    } else if (configuration_step_ == write_count) {
      // CTRL_REG1=10 Hz is the final configuration write. LIS3DH high-resolution
      // output requires 7/ODR turn-on time; do not inspect DRDY or output data
      // until that interval has elapsed, even if retained status is already set.
      next_action_at_ms_ = now + accelerometer_config::kHighResolutionSettleMs;
      state_ = State::kHighResolutionSettling;
    }
    return Event::kNone;
  }

  if (state_ == State::kHighResolutionSettling) {
    if (!monotonic::reached(now, next_action_at_ms_)) return Event::kNone;
    probe_started_at_ms_ = now;
    next_action_at_ms_ = now;
    state_ = State::kProbeWait;
    return Event::kNone;
  }

  if (state_ == State::kProbeWait) {
    if (!monotonic::reached(now, next_action_at_ms_)) return Event::kNone;

    // Backpressure never overwrites a sample. An abandoned handoff fails closed
    // after the normal sample timeout instead of leaving the sensor on forever.
    if (runtime_active_ && runtime_sample_ready_) {
      if (monotonic::elapsed(now, probe_started_at_ms_,
                             accelerometer_config::kProbeTimeoutMs)) {
        ++diagnostics_.sample_failures;
        startPowerDown(Event::kFault);
      }
      return Event::kNone;
    }
    uint8_t status = 0;
    const BusResult result = readRegister(kStatus, &status);
    accountBusResult(result, diagnostics_, saw_transport_timeout_);
    if (result != BusResult::kOk) {
      ++diagnostics_.sample_failures;
      startPowerDown(Event::kFault);
      return Event::kNone;
    }

    if ((status & kStatusXyzDataAvailable) == 0) {
      if (monotonic::elapsed(now, probe_started_at_ms_,
                             accelerometer_config::kProbeTimeoutMs)) {
        ++diagnostics_.sample_failures;
        startPowerDown(Event::kFault);
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
      startPowerDown(Event::kFault);
      return Event::kNone;
    }

    if (discard_next_sample_) {
      // LIS3DH retains the previous output registers in power-down mode. Reading
      // and discarding the first ready set prevents a pre-reset/pre-DFU sample
      // from being reported as a fresh M6A observation. After HR settling, wait
      // one additional ODR period and give that fresh phase its own timeout budget.
      discard_next_sample_ = false;
      probe_started_at_ms_ = now;
      next_action_at_ms_ = now + accelerometer_config::kProbeSamplePeriodMs;
      state_ = State::kProbeWait;
      return Event::kNone;
    }

    const AccelerometerSample sample(
        now, highResolution2gToMg(bytes[0], bytes[1]),
        highResolution2gToMg(bytes[2], bytes[3]),
        highResolution2gToMg(bytes[4], bytes[5]));
    if (runtime_active_) {
      runtime_sample_ = sample;
      runtime_sample_ready_ = true;
      probe_started_at_ms_ = now;
      next_action_at_ms_ = now + accelerometer_config::kProbeSamplePeriodMs;
      state_ = State::kProbeWait;
      return Event::kNone;
    }
    probe_sample_ = sample;
    ++diagnostics_.probe_samples;
    startPowerDown(Event::kPresent);
    return Event::kNone;
  }

  if (state_ == State::kPoweringDown) {
    ++power_down_attempts_;
    if (!powerDownSensor(diagnostics_, saw_transport_timeout_)) {
      if (power_down_attempts_ < accelerometer_config::kPowerDownMaxAttempts)
        return Event::kNone;
      return enterFaultCleanup(now);
    }
    if ((runtime_active_ || stream_active_) &&
        pending_finish_event_ == Event::kNone) {
      runtime_active_ = false;
      stream_active_ = false;
      state_ = State::kDone;
      return Event::kNone;
    }
    if (pending_finish_event_ == Event::kPresent)
      return finishPresent();
    return finishFault();
  }

  if (state_ == State::kFaultCleanup) {
    if (!monotonic::reached(now, next_action_at_ms_)) return Event::kNone;
    if (powerDownSensor(diagnostics_, saw_transport_timeout_)) {
      state_ = State::kDone;
      return Event::kNone;
    }
    next_action_at_ms_ = now + accelerometer_config::kFaultCleanupRetryBackoffMs;
    return Event::kNone;
  }

  return Event::kNone;
}

bool AccelerometerManager::startRuntimeSession() {
  if (!runtimeShutdownConfirmed()) return false;
  runtime_active_ = true;
  runtime_sample_ready_ = false;
  discard_next_sample_ = true;
  configuration_step_ = 0;
  state_ = State::kConfiguring;
  return true;
}

bool AccelerometerManager::takeRuntimeSample(AccelerometerSample* sample) {
  if (sample == nullptr || !runtime_sample_ready_ || faulted_) return false;
  *sample = runtime_sample_;
  runtime_sample_ready_ = false;
  return true;
}

void AccelerometerManager::stopRuntimeSession() {
  // Repeated stop must not restart the three-attempt shutdown budget or mask a
  // pending fault. Fault cleanup always retains its original owner.
  if (runtime_active_ && state_ != State::kPoweringDown)
    startPowerDown(Event::kNone);
}

bool AccelerometerManager::takeProbeSample(AccelerometerSample* sample) {
  if (sample == nullptr || !probe_sample_ready_) return false;
  *sample = probe_sample_;
  probe_sample_ready_ = false;
  return true;
}

bool AccelerometerManager::startStream() {
  if (!runtimeShutdownConfirmed()) return false;
  stream_active_ = true;
  stream_break_pending_ = true;
  stream_consecutive_errors_ = 0;
  clearStreamBuffer();
  configuration_step_ = 0;
  state_ = State::kConfiguring;
  return true;
}

void AccelerometerManager::stopStream() {
  // Like stopRuntimeSession(): repeated stop keeps the shutdown budget.
  if (stream_active_ && state_ != State::kPoweringDown &&
      state_ != State::kFaultCleanup)
    startPowerDown(Event::kNone);
}

bool AccelerometerManager::takeStreamSample(AccelerometerStreamSample* sample) {
  if (sample == nullptr || stream_count_ == 0 || faulted_) return false;
  *sample = stream_buffer_[stream_head_];
  stream_head_ = static_cast<uint8_t>((stream_head_ + 1U) %
                                      accelerometer_config::kFifoCapacity);
  --stream_count_;
  return true;
}

void AccelerometerManager::clearStreamBuffer() {
  stream_head_ = 0;
  stream_count_ = 0;
}

void AccelerometerManager::pushStreamSample(const AccelerometerSample& sample) {
  if (stream_count_ == accelerometer_config::kFifoCapacity) {
    // The consumer fell a whole FIFO behind: keep nothing that would hide
    // the loss; the next kept sample starts after a break.
    ++diagnostics_.stream_overruns;
    stream_break_pending_ = true;
    return;
  }
  const uint8_t tail = static_cast<uint8_t>(
      (stream_head_ + stream_count_) % accelerometer_config::kFifoCapacity);
  stream_buffer_[tail].sample = sample;
  stream_buffer_[tail].continuity_break = stream_break_pending_;
  stream_break_pending_ = false;
  ++stream_count_;
}

AccelerometerManager::Event AccelerometerManager::streamDrainFailed(
    uint32_t now) {
  ++diagnostics_.stream_read_errors;
  stream_break_pending_ = true;
  if (++stream_consecutive_errors_ >=
      accelerometer_config::kStreamMaxConsecutiveErrors) {
    startPowerDown(Event::kFault);
    return Event::kNone;
  }
  next_action_at_ms_ = now + accelerometer_config::kStreamDrainPeriodMs;
  state_ = State::kStreamWaiting;
  return Event::kNone;
}

// One bus operation per pass, like configuration: select Stream mode after the
// settle, then every drain period read FIFO_SRC, then the stored samples in
// reads of at most kStreamMaxSamplesPerRead.
AccelerometerManager::Event AccelerometerManager::pollStream(uint32_t now) {
  if (state_ == State::kStreamSettling) {
    if (!monotonic::reached(now, next_action_at_ms_)) return Event::kNone;
    const BusResult result = writeRegister(kFifoCtrl, kFifoModeStream);
    accountBusResult(result, diagnostics_, saw_transport_timeout_);
    if (result != BusResult::kOk) {
      ++diagnostics_.configuration_failures;
      startPowerDown(Event::kFault);
      return Event::kNone;
    }
    last_drain_at_ms_ = now;
    next_action_at_ms_ = now + accelerometer_config::kStreamDrainPeriodMs;
    state_ = State::kStreamWaiting;
    return Event::kNone;
  }

  if (state_ == State::kStreamWaiting) {
    if (!monotonic::reached(now, next_action_at_ms_)) return Event::kNone;
    uint8_t source = 0;
    const BusResult result = readRegister(kFifoSrc, &source);
    accountBusResult(result, diagnostics_, saw_transport_timeout_);
    if (result == BusResult::kTimeoutFailed) {
      startPowerDown(Event::kFault);
      return Event::kNone;
    }
    if (result != BusResult::kOk) return streamDrainFailed(now);

    ++diagnostics_.stream_drains;
    const uint32_t elapsed_ms = now - last_drain_at_ms_;
    last_drain_at_ms_ = now;
    const uint8_t stored = source & kFifoSrcLevelMask;
    if ((source & kFifoSrcOverrun) != 0) {
      // Late drain: the oldest samples were overwritten. The stored ones are
      // still contiguous among themselves, so keep them after a break.
      ++diagnostics_.stream_overruns;
      stream_break_pending_ = true;
      stream_consecutive_errors_ = 0;
    } else if (!streamCountPlausible(stored, elapsed_ms)) {
      // Not sampling at 10 Hz: read the samples out but do not keep them.
      ++diagnostics_.stream_timing_errors;
      stream_break_pending_ = true;
      if (++stream_consecutive_errors_ >=
          accelerometer_config::kStreamMaxConsecutiveErrors) {
        startPowerDown(Event::kFault);
        return Event::kNone;
      }
    } else {
      stream_consecutive_errors_ = 0;
    }

    next_action_at_ms_ = now + accelerometer_config::kStreamDrainPeriodMs;
    if (stored == 0) return Event::kNone;
    drain_at_ms_ = now;
    drain_total_ = stored;
    drain_read_ = 0;
    state_ = State::kStreamReading;
    return Event::kNone;
  }

  // kStreamReading
  const uint8_t remaining = static_cast<uint8_t>(drain_total_ - drain_read_);
  const uint8_t batch =
      remaining < accelerometer_config::kStreamMaxSamplesPerRead
          ? remaining
          : accelerometer_config::kStreamMaxSamplesPerRead;
  uint8_t bytes[accelerometer_config::kStreamMaxSamplesPerRead * 6U]{};
  const BusResult result =
      readAxes(bytes, static_cast<uint8_t>(batch * 6U));
  accountBusResult(result, diagnostics_, saw_transport_timeout_);
  if (result == BusResult::kTimeoutFailed) {
    startPowerDown(Event::kFault);
    return Event::kNone;
  }
  if (result != BusResult::kOk) return streamDrainFailed(now);

  // The error count is nonzero here only when this drain's sample count was
  // implausible: such samples are read out (the FIFO keeps its phase) but
  // not kept.
  const bool keep = stream_consecutive_errors_ == 0;
  for (uint8_t i = 0; keep && i < batch; ++i) {
    const uint32_t index = drain_read_ + i;
    const uint32_t age_ms = (drain_total_ - 1U - index) *
                            accelerometer_config::kProbeSamplePeriodMs;
    const uint8_t* axes = bytes + 6U * i;
    pushStreamSample(AccelerometerSample(
        drain_at_ms_ - age_ms, normal2gToMg(axes[0], axes[1]),
        normal2gToMg(axes[2], axes[3]), normal2gToMg(axes[4], axes[5])));
  }
  if (keep) diagnostics_.stream_samples += batch;
  drain_read_ = static_cast<uint8_t>(drain_read_ + batch);
  if (drain_read_ == drain_total_) state_ = State::kStreamWaiting;
  return Event::kNone;
}

}  // namespace orun_tlp
