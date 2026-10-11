#pragma once

#include <stdint.h>

namespace orun_tlp {
namespace accelerometer_config {

constexpr uint8_t kI2cAddress = 0x18;
constexpr uint8_t kWhoAmIValue = 0x33;

constexpr uint16_t kProbeSampleRateHz = 10;
constexpr uint32_t kProbeSamplePeriodMs = 1000UL / kProbeSampleRateHz;
// LIS3DH high-resolution mode requires 7/ODR turn-on time. Keep this separate
// from the normal data-ready timeout so an early retained/transition sample
// cannot be accepted merely because ZYXDA is already asserted.
constexpr uint8_t kHighResolutionTurnOnPeriods = 7;
constexpr uint32_t kHighResolutionSettleMs =
    kProbeSamplePeriodMs * kHighResolutionTurnOnPeriods;
constexpr uint32_t kProbeTimeoutMs = 500;

constexpr uint8_t kDetectionMaxAttempts = 3;
constexpr uint32_t kDetectionRetryBackoffMs = 250;
constexpr uint8_t kPowerDownMaxAttempts = 3;
// Once the immediate shutdown budget is exhausted, keep the capability faulted
// but retry one cleanup write only once per minute. This avoids abandoning a
// positively identified sensor in 10 Hz mode while also avoiding a tight shared-
// I2C recovery loop that could interfere with GNSS.
constexpr uint32_t kFaultCleanupRetryBackoffMs = 60UL * 1000UL;

// Continuous activity stream (owner decision 2026-10-12: sample all the time,
// as other collars do, instead of 5 s every 3 min). LIS3DH FIFO in Stream
// mode at the same 10 Hz. The FIFO holds 10-bit samples (ST AN3308, FIFO
// section), so the stream uses normal mode, not high resolution. 32 levels
// hold 3.2 s; draining every 2 s leaves 1.2 s of slack. A later drain still
// loses nothing silently: the sensor flags the overrun and the consumer
// treats the break as unmeasured time.
constexpr uint8_t kFifoCapacity = 32;
constexpr uint32_t kStreamDrainPeriodMs = 2000;
// One I2C read carries at most 10 samples: 60 bytes fit the 64-byte Wire
// receive buffer of the Adafruit nRF52 core, which does not bound the length.
constexpr uint8_t kStreamMaxSamplesPerRead = 10;
// Normal-mode turn-on is 1.6 ms. The FIFO stays in Bypass (stores nothing)
// for two output periods after the 10 Hz write, so no turn-on sample is kept.
constexpr uint32_t kStreamSettleMs = 2UL * kProbeSamplePeriodMs;
// Failed or implausible drains in a row before the stream is a sensor fault.
constexpr uint8_t kStreamMaxConsecutiveErrors = 3;

static_assert(kStreamDrainPeriodMs / kProbeSamplePeriodMs < kFifoCapacity,
              "the stream must be drained before the FIFO fills");
static_assert(kStreamMaxSamplesPerRead * 6U <= 64U,
              "one FIFO read must fit the Wire receive buffer");

static_assert(1000UL % kProbeSampleRateHz == 0,
              "accelerometer sample period must be integral milliseconds");
static_assert(kHighResolutionSettleMs >= 7UL * kProbeSamplePeriodMs,
              "high-resolution settling must cover LIS3DH 7/ODR turn-on");
static_assert(kPowerDownMaxAttempts > 0,
              "accelerometer power-down must be attempted at least once");

}  // namespace accelerometer_config
}  // namespace orun_tlp
