#pragma once

#include <stdint.h>

namespace orun_tlp::radio_config {

// M1 defaults. These values will move behind ConfigManager in a later milestone.
constexpr uint32_t kFrequencyHz = 869525000;
constexpr int8_t kTxPowerDbm = 14;
constexpr uint8_t kBandwidth = 0;       // SX126x-Arduino: 0 = 125 kHz
constexpr uint16_t kBandwidthKhz = 125;
constexpr uint8_t kSpreadingFactor = 11;
constexpr uint8_t kCodingRate = 1;      // SX126x-Arduino: 1 = 4/5
constexpr uint8_t kCodingRateNumerator = 4;
constexpr uint8_t kCodingRateDenominator = 5;
constexpr uint16_t kPreambleLength = 8;
constexpr bool kCrcEnabled = true;
constexpr bool kExplicitHeader = true;
constexpr bool kIqInverted = false;
constexpr uint16_t kPrivateSyncWord = 0x1424;
constexpr uint32_t kTxTimeoutMs = 5000;

// M6P1: bounded post-TX listen window for nodes without an availability
// commitment. Not tuned; reserved for future ACK/downlink. Volatile constant,
// not persisted configuration.
constexpr uint32_t kWindowedRxAfterTxMs = 3000;
static_assert(kWindowedRxAfterTxMs < 0x80000000UL,
              "windowed RX deadline must fit monotonic half-range");

// SX126x-Arduino automatically enables Low Data Rate Optimization for SF11 or
// SF12 at BW125. Do not call Radio.EnforceLowDRopt() for this M1 profile.

// M1 test cadence only; it is not a future tracker reporting policy.
constexpr bool kTestBeaconEnabled = false;
constexpr uint32_t kTestIntervalMs = 30000;
constexpr uint32_t kInitialOffsetRangeMs = 5000;
constexpr uint32_t kPerPacketJitterRangeMs = 1500;

// TX power is conducted power. Regulatory limits and antenna ERP must be
// validated for the installed antenna and deployment jurisdiction before use.
// kPrivateSyncWord is the library/Semtech private LoRa value, not a secret.

}  // namespace orun_tlp::radio_config
