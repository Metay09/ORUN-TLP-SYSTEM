#pragma once

// SF5B PRODUCT_SECURE test vectors, shared by the host codec test and the
// RAK4631 KAT probe. Every hex string is pinned by the independent generator
// test_sf5b_product_secure_vectors.py (struct.pack + HKDF-SHA256 +
// AES-128-CCM); change them only together with that script.
//
// gnu++11-compatible: the probe builds with the target toolchain.

#include <stddef.h>
#include <stdint.h>

#include "tlp_v2_product_secure.h"

namespace orun_tlp::sf5b_vectors {

// Public test credential, identical to the SF2 HISTORY_SECURE vectors.
constexpr uint64_t kDeviceId = 0x1122334455667788ULL;
constexpr uint32_t kKeyEpoch = 0x01020304UL;

static const uint8_t kCredentialId[16] = {
    0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7,
    0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF};
static const uint8_t kRoot[32] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F};

constexpr uint8_t kPathFlags = tlp::kProductPathFlagRelayAllowed |
                               tlp::kProductPathFlagCustodyRequested;

static const char kPeriodicPlaintextHex[] =
    "017f0101a1a2a3a4a5a6a7a8000001016acba47000000e10166adf3e10b8c704"
    "0001e84800ba000c0501010000000ce400000384000e00250fac000101000000";
// Amendment A1: BEHAVIOUR_VALID with head_down_share = 72 (36 %).
static const char kPeriodicBehaviourPlaintextHex[] =
    "01ff0101a1a2a3a4a5a6a7a8000001016acba47000000e10166adf3e10b8c704"
    "0001e84800ba000c0501014800000ce400000384000e00250fac000101000000";
static const char kEventPlaintextHex[] =
    "0101010207000100a1a2a3a4a5a6a7a8000001026acba218b1b2b3b4b5b6b7b8"
    "166b97f010b99dc0001e000000000000";
// Host golden frames: counters 0x42 / 0x43.
static const char kPeriodicFrameHex[] =
    "0204020103014000112233445566778801020304000000000000004"
    "20a19fe1a180d483149c11e413e3944c95bf8152d9d888ad7b4f16fc0bd0732"
    "07543030490747856b73c6294bec5cac9962ef666a45aeed52a37e70c12790c0"
    "e5283b12ba5d38d03f";
static const char kEventFrameHex[] =
    "0204020203013000112233445566778801020304000000000000004"
    "3bbd99aa262fc4aed44203fb2c0a9e6e32815ee72f5b0a03dcc34a22b153b66"
    "ea9269a9a8428c27dbaebfcf615154d9158e90b4be8a6887f5";
// KAT frames: a freshly provisioned SecurityStore burns counter 0, so the
// first two protected records use counters 1 and 2.
static const char kKatPeriodicFrameHex[] =
    "02040201030140001122334455667788010203040000000000000001"
    "e50cbc0f761c6a0eb03b458063cb18b8f077576bc1a2a7e4768c6838f05cbcce"
    "b5e1c5ebea866295c57085068185f0f6a698df279a8a7d2b5e9f9b7beda0a3f9"
    "b3417dfed60d254d";
static const char kKatEventFrameHex[] =
    "02040202030130001122334455667788010203040000000000000002"
    "527c4362b1049d40724479242eff9cbc3746611b09d93b737d325ac3b884ea99"
    "96b2d4c7019183c5cc8562073586d139598a349b548bf7c6";

inline uint8_t hexNibble(char c) {
  if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
  if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
  if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
  return 0xFF;
}

// Parses exactly `size` bytes; false on a short or non-hex string.
inline bool fromHex(const char* hex, uint8_t* out, size_t size) {
  for (size_t i = 0; i < size; ++i) {
    const uint8_t high = hexNibble(hex[2 * i]);
    if (high == 0xFF) return false;
    const uint8_t low = hexNibble(hex[2 * i + 1]);
    if (low == 0xFF) return false;
    out[i] = static_cast<uint8_t>((high << 4) | low);
  }
  return hex[2 * size] == '\0';
}

inline tlp::PeriodicObservationPlaintext periodic() {
  tlp::PeriodicObservationPlaintext p;
  p.record_flags = 0x7F;
  p.location_source = tlp::kLocationSourceGnss;
  p.battery_state = 1;  // NORMAL
  p.record_incarnation = 0xA1A2A3A4A5A6A7A8ULL;
  p.record_sequence = 257;
  p.period_end_epoch_seconds = 1791730800UL;
  p.period_duration_seconds = 3600;
  p.latitude_e7 = 376102718;
  p.longitude_e7 = 280545028;
  p.altitude_mm = 125000;
  p.hdop_x100 = 186;
  p.location_age_seconds = 12;
  p.satellites = 5;
  p.activity_quality = tlp::kActivityQualityPartial;
  p.battery_quality = tlp::kBatteryQualityMeasured;
  p.activity_coverage_seconds = 3300;
  p.active_seconds = 900;
  p.activity_transition_count = 14;
  p.movement_mean_abs_delta_mg = 37;
  p.battery_mv = 4012;
  p.health_flags = tlp::kHealthGnssDegraded;
  p.gnss_failure_count = 1;
  return p;
}

inline tlp::EventPlaintext event() {
  tlp::EventPlaintext e;
  e.event_type = tlp::kEventTypeGeofenceOutside;
  e.lifecycle_state = tlp::kEventLifecycleActive;
  e.severity = tlp::kEventSeverityWarning;
  e.event_flags = tlp::kEventTransitionTimeValid | tlp::kEventLocationValid |
                  tlp::kEventLocationAgeValid;
  e.location_source = tlp::kLocationSourceGnss;
  e.record_incarnation = 0xA1A2A3A4A5A6A7A8ULL;
  e.record_sequence = 258;
  e.transition_epoch_seconds = 1791730800UL - 600;
  e.occurrence_id = 0xB1B2B3B4B5B6B7B8ULL;
  e.latitude_e7 = 376150000;
  e.longitude_e7 = 280600000;
  e.location_age_seconds = 30;
  return e;
}

}  // namespace orun_tlp::sf5b_vectors
