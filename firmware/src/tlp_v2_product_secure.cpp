#include "tlp_v2_product_secure.h"

#include <string.h>

namespace orun_tlp::tlp {
namespace {

using R = ProductSecureCodecResult;

constexpr int32_t kMaxLatitudeE7 = 900000000;
constexpr int32_t kMaxLongitudeE7 = 1800000000;

void putU16(uint8_t* out, uint16_t value) {
  out[0] = static_cast<uint8_t>(value >> 8);
  out[1] = static_cast<uint8_t>(value);
}

void putU32(uint8_t* out, uint32_t value) {
  out[0] = static_cast<uint8_t>(value >> 24);
  out[1] = static_cast<uint8_t>(value >> 16);
  out[2] = static_cast<uint8_t>(value >> 8);
  out[3] = static_cast<uint8_t>(value);
}

void putU64(uint8_t* out, uint64_t value) {
  putU32(out, static_cast<uint32_t>(value >> 32));
  putU32(out + 4, static_cast<uint32_t>(value));
}

uint16_t getU16(const uint8_t* in) {
  return static_cast<uint16_t>((static_cast<uint16_t>(in[0]) << 8) | in[1]);
}

uint32_t getU32(const uint8_t* in) {
  return (static_cast<uint32_t>(in[0]) << 24) |
         (static_cast<uint32_t>(in[1]) << 16) |
         (static_cast<uint32_t>(in[2]) << 8) | static_cast<uint32_t>(in[3]);
}

uint64_t getU64(const uint8_t* in) {
  return (static_cast<uint64_t>(getU32(in)) << 32) | getU32(in + 4);
}

void putI32(uint8_t* out, int32_t value) {
  putU32(out, static_cast<uint32_t>(value));
}

int32_t getI32(const uint8_t* in) {
  return static_cast<int32_t>(getU32(in));
}

bool coordinatesInRange(int32_t latitude_e7, int32_t longitude_e7) {
  return latitude_e7 >= -kMaxLatitudeE7 && latitude_e7 <= kMaxLatitudeE7 &&
         longitude_e7 >= -kMaxLongitudeE7 && longitude_e7 <= kMaxLongitudeE7;
}

size_t familyCiphertextSize(uint8_t app_family) {
  if (app_family == kProductAppFamilyPeriodic) return kPeriodicPlaintextSize;
  if (app_family == kProductAppFamilyEvent) return kEventPlaintextSize;
  return 0;
}

// Header rules shared by the encoder, the AAD builder and (after the fixed
// version/type/context/reserved bytes) the decoder.
R checkHeaderFields(const ProductSecureFrame& frame) {
  if (frame.path_flags & static_cast<uint8_t>(~kProductPathFlagsAllowedMask))
    return R::kBadPathFlags;
  const size_t ciphertext_size = familyCiphertextSize(frame.app_family);
  if (ciphertext_size == 0) return R::kBadFamily;
  if (frame.security_profile != kProductSecurityProfileRootAeadV1)
    return R::kBadProfile;
  if (frame.ciphertext_len != ciphertext_size) return R::kBadCiphertextLength;
  if (frame.key_epoch == UINT32_MAX) return R::kBadEpoch;
  if (frame.security_counter == 0) return R::kBadCounter;
  return R::kOk;
}

void writeHeader(const ProductSecureFrame& frame, uint8_t* out) {
  out[0] = kProductSecureVersion;
  out[1] = kPacketTypeProductSecure;
  out[2] = kProductSecurityContextDeviceD2a;
  out[3] = frame.app_family;
  out[4] = frame.path_flags;
  out[5] = frame.security_profile;
  out[6] = frame.ciphertext_len;
  out[7] = 0;
  putU64(out + 8, frame.device_id);
  putU32(out + 16, frame.key_epoch);
  putU64(out + 20, frame.security_counter);
}

}  // namespace

const char* productSecureCodecResultName(ProductSecureCodecResult result) {
  switch (result) {
    case R::kOk: return "OK";
    case R::kInvalidArgument: return "INVALID_ARGUMENT";
    case R::kBadLength: return "BAD_LENGTH";
    case R::kBadVersionOrType: return "BAD_VERSION_OR_TYPE";
    case R::kBadContext: return "BAD_CONTEXT";
    case R::kBadFamily: return "BAD_FAMILY";
    case R::kBadProfile: return "BAD_PROFILE";
    case R::kBadPathFlags: return "BAD_PATH_FLAGS";
    case R::kBadReserved: return "BAD_RESERVED";
    case R::kBadCiphertextLength: return "BAD_CIPHERTEXT_LENGTH";
    case R::kBadEpoch: return "BAD_EPOCH";
    case R::kBadCounter: return "BAD_COUNTER";
    case R::kBadSchema: return "BAD_SCHEMA";
    case R::kBadFlags: return "BAD_FLAGS";
    case R::kBadEnum: return "BAD_ENUM";
    case R::kBadIdentity: return "BAD_IDENTITY";
    case R::kBadTime: return "BAD_TIME";
    case R::kBadLocation: return "BAD_LOCATION";
    case R::kBadActivity: return "BAD_ACTIVITY";
    case R::kBadBattery: return "BAD_BATTERY";
    case R::kBadHealth: return "BAD_HEALTH";
    case R::kBadContextValue: return "BAD_CONTEXT_VALUE";
  }
  return "UNKNOWN";
}

ProductSecureCodecResult validatePeriodicPlaintext(
    const PeriodicObservationPlaintext& p) {
  const uint8_t flags = p.record_flags;
  if (flags & static_cast<uint8_t>(~kPeriodicRecordFlagsAllowedMask))
    return R::kBadFlags;
  if (p.location_source > kLocationSourceManual ||
      p.activity_quality > kActivityQualityComplete ||
      p.battery_state > kBatteryStateCritical ||
      p.battery_quality > kBatteryQualityModelEstimated)
    return R::kBadEnum;
  if (p.record_incarnation == 0 || p.record_sequence == 0)
    return R::kBadIdentity;

  // 7.1: the period always has a length; its end time is optional.
  if (p.period_duration_seconds == 0) return R::kBadTime;
  const bool time_valid = (flags & kPeriodicPeriodTimeValid) != 0;
  if (time_valid != (p.period_end_epoch_seconds != 0)) return R::kBadTime;

  // 7.1/7.2: canonical location; stale values never survive a clear bit.
  const bool location_valid = (flags & kPeriodicLocationValid) != 0;
  const bool altitude_valid = (flags & kPeriodicAltitudeValid) != 0;
  const bool age_valid = (flags & kPeriodicLocationAgeValid) != 0;
  if (!location_valid) {
    if (p.location_source != kLocationSourceUnknown || altitude_valid ||
        age_valid || p.latitude_e7 != 0 || p.longitude_e7 != 0 ||
        p.altitude_mm != 0 || p.hdop_x100 != 0 || p.satellites != 0 ||
        p.location_age_seconds != kLocationAgeUnknown)
      return R::kBadLocation;
  } else {
    if (p.location_source == kLocationSourceUnknown ||
        !coordinatesInRange(p.latitude_e7, p.longitude_e7))
      return R::kBadLocation;
    if (!altitude_valid && p.altitude_mm != 0) return R::kBadLocation;
    if (age_valid ? p.location_age_seconds > kLocationAgeSaturated
                  : p.location_age_seconds != kLocationAgeUnknown)
      return R::kBadLocation;
    if (p.location_source != kLocationSourceGnss &&
        (p.hdop_x100 != 0 || p.satellites != 0))
      return R::kBadLocation;
  }

  // 7.3: coverage decides the quality; a gap is never complete coverage.
  if ((flags & kPeriodicActivityValid) == 0) {
    if (p.activity_coverage_seconds != 0 || p.active_seconds != 0 ||
        p.activity_transition_count != 0 || p.movement_mean_abs_delta_mg != 0 ||
        p.activity_quality != kActivityQualityUnknown)
      return R::kBadActivity;
  } else {
    if (p.activity_coverage_seconds == 0 ||
        p.active_seconds > p.activity_coverage_seconds ||
        p.activity_coverage_seconds > p.period_duration_seconds)
      return R::kBadActivity;
    const uint8_t expected =
        p.activity_coverage_seconds == p.period_duration_seconds
            ? kActivityQualityComplete
            : kActivityQualityPartial;
    if (p.activity_quality != expected) return R::kBadActivity;
  }

  // 7.3 amendment A1: head position needs activity, canonical zero without.
  if ((flags & kPeriodicBehaviourValid) == 0) {
    if (p.head_down_share != 0) return R::kBadActivity;
  } else {
    if ((flags & kPeriodicActivityValid) == 0 ||
        p.head_down_share > kPeriodicHeadDownShareMax ||
        (p.active_seconds == 0 && p.head_down_share != 0))
      return R::kBadActivity;
  }

  // 7.4
  if ((flags & kPeriodicBatteryValid) == 0 &&
      (p.battery_mv != 0 || p.battery_state != kBatteryStateUnknown ||
       p.battery_quality != kBatteryQualityUnknown))
    return R::kBadBattery;

  // 7.5
  if (p.health_flags & static_cast<uint16_t>(~kHealthFlagsAllowedMask))
    return R::kBadHealth;
  if ((flags & kPeriodicHealthValid) == 0 &&
      (p.health_flags != 0 || p.gnss_failure_count != 0 ||
       p.sensor_failure_count != 0 || p.storage_failure_count != 0 ||
       p.radio_failure_count != 0))
    return R::kBadHealth;
  return R::kOk;
}

ProductSecureCodecResult validateEventPlaintext(const EventPlaintext& e) {
  if (e.event_type < kEventTypeGeofenceOutside ||
      e.event_type > kEventTypeSubsystemFault ||
      e.lifecycle_state < kEventLifecycleActive ||
      e.lifecycle_state > kEventLifecycleCleared ||
      e.severity < kEventSeverityInfo || e.severity > kEventSeverityCritical ||
      e.location_source > kLocationSourceManual ||
      e.context_kind > kEventContextSubsystemId)
    return R::kBadEnum;
  // 8.4: only BATTERY_STATE assigns reason codes in schema v1.
  if (e.event_type == kEventTypeBatteryState
          ? (e.reason_code != kBatteryReasonLow &&
             e.reason_code != kBatteryReasonCritical)
          : e.reason_code != 0)
    return R::kBadEnum;
  if (e.event_flags & static_cast<uint8_t>(~kEventFlagsAllowedMask))
    return R::kBadFlags;
  if (e.record_incarnation == 0 || e.record_sequence == 0 ||
      e.occurrence_id == 0)
    return R::kBadIdentity;

  const bool time_valid = (e.event_flags & kEventTransitionTimeValid) != 0;
  if (time_valid != (e.transition_epoch_seconds != 0)) return R::kBadTime;

  const bool location_valid = (e.event_flags & kEventLocationValid) != 0;
  const bool age_valid = (e.event_flags & kEventLocationAgeValid) != 0;
  if (!location_valid) {
    if (e.location_source != kLocationSourceUnknown || age_valid ||
        e.latitude_e7 != 0 || e.longitude_e7 != 0 ||
        e.location_age_seconds != kLocationAgeUnknown)
      return R::kBadLocation;
  } else {
    if (e.location_source == kLocationSourceUnknown ||
        !coordinatesInRange(e.latitude_e7, e.longitude_e7))
      return R::kBadLocation;
    if (age_valid ? e.location_age_seconds > kLocationAgeSaturated
                  : e.location_age_seconds != kLocationAgeUnknown)
      return R::kBadLocation;
  }

  if (e.context_flags != 0) return R::kBadContextValue;
  if ((e.event_flags & kEventContextValid) == 0
          ? (e.context_kind != kEventContextNone || e.context_value != 0)
          : e.context_kind == kEventContextNone)
    return R::kBadContextValue;
  return R::kOk;
}

ProductSecureCodecResult encodePeriodicPlaintext(
    const PeriodicObservationPlaintext& p, uint8_t* out, size_t out_size) {
  if (out == nullptr) return R::kInvalidArgument;
  if (out_size != kPeriodicPlaintextSize) return R::kBadLength;
  const R valid = validatePeriodicPlaintext(p);
  if (valid != R::kOk) return valid;
  out[0] = kPeriodicSchema;
  out[1] = p.record_flags;
  out[2] = p.location_source;
  out[3] = p.battery_state;
  putU64(out + 4, p.record_incarnation);
  putU32(out + 12, p.record_sequence);
  putU32(out + 16, p.period_end_epoch_seconds);
  putU32(out + 20, p.period_duration_seconds);
  putI32(out + 24, p.latitude_e7);
  putI32(out + 28, p.longitude_e7);
  putI32(out + 32, p.altitude_mm);
  putU16(out + 36, p.hdop_x100);
  putU16(out + 38, p.location_age_seconds);
  out[40] = p.satellites;
  out[41] = p.activity_quality;
  out[42] = p.battery_quality;
  out[43] = p.head_down_share;
  putU32(out + 44, p.activity_coverage_seconds);
  putU32(out + 48, p.active_seconds);
  putU16(out + 52, p.activity_transition_count);
  putU16(out + 54, p.movement_mean_abs_delta_mg);
  putU16(out + 56, p.battery_mv);
  putU16(out + 58, p.health_flags);
  out[60] = p.gnss_failure_count;
  out[61] = p.sensor_failure_count;
  out[62] = p.storage_failure_count;
  out[63] = p.radio_failure_count;
  return R::kOk;
}

ProductSecureCodecResult decodePeriodicPlaintext(
    const uint8_t* in, size_t in_size, PeriodicObservationPlaintext* output) {
  if (in == nullptr || output == nullptr) return R::kInvalidArgument;
  if (in_size != kPeriodicPlaintextSize) return R::kBadLength;
  if (in[0] != kPeriodicSchema) return R::kBadSchema;
  PeriodicObservationPlaintext p;
  p.record_flags = in[1];
  p.location_source = in[2];
  p.battery_state = in[3];
  p.record_incarnation = getU64(in + 4);
  p.record_sequence = getU32(in + 12);
  p.period_end_epoch_seconds = getU32(in + 16);
  p.period_duration_seconds = getU32(in + 20);
  p.latitude_e7 = getI32(in + 24);
  p.longitude_e7 = getI32(in + 28);
  p.altitude_mm = getI32(in + 32);
  p.hdop_x100 = getU16(in + 36);
  p.location_age_seconds = getU16(in + 38);
  p.satellites = in[40];
  p.activity_quality = in[41];
  p.battery_quality = in[42];
  p.head_down_share = in[43];
  p.activity_coverage_seconds = getU32(in + 44);
  p.active_seconds = getU32(in + 48);
  p.activity_transition_count = getU16(in + 52);
  p.movement_mean_abs_delta_mg = getU16(in + 54);
  p.battery_mv = getU16(in + 56);
  p.health_flags = getU16(in + 58);
  p.gnss_failure_count = in[60];
  p.sensor_failure_count = in[61];
  p.storage_failure_count = in[62];
  p.radio_failure_count = in[63];
  const R valid = validatePeriodicPlaintext(p);
  if (valid != R::kOk) return valid;
  *output = p;
  return R::kOk;
}

ProductSecureCodecResult encodeEventPlaintext(const EventPlaintext& e,
                                              uint8_t* out, size_t out_size) {
  if (out == nullptr) return R::kInvalidArgument;
  if (out_size != kEventPlaintextSize) return R::kBadLength;
  const R valid = validateEventPlaintext(e);
  if (valid != R::kOk) return valid;
  out[0] = kEventSchema;
  out[1] = e.event_type;
  out[2] = e.lifecycle_state;
  out[3] = e.severity;
  out[4] = e.event_flags;
  out[5] = e.reason_code;
  out[6] = e.location_source;
  out[7] = e.context_kind;
  putU64(out + 8, e.record_incarnation);
  putU32(out + 16, e.record_sequence);
  putU32(out + 20, e.transition_epoch_seconds);
  putU64(out + 24, e.occurrence_id);
  putI32(out + 32, e.latitude_e7);
  putI32(out + 36, e.longitude_e7);
  putU16(out + 40, e.location_age_seconds);
  putU16(out + 42, e.context_flags);
  putU32(out + 44, e.context_value);
  return R::kOk;
}

ProductSecureCodecResult decodeEventPlaintext(const uint8_t* in,
                                              size_t in_size,
                                              EventPlaintext* output) {
  if (in == nullptr || output == nullptr) return R::kInvalidArgument;
  if (in_size != kEventPlaintextSize) return R::kBadLength;
  if (in[0] != kEventSchema) return R::kBadSchema;
  EventPlaintext e;
  e.event_type = in[1];
  e.lifecycle_state = in[2];
  e.severity = in[3];
  e.event_flags = in[4];
  e.reason_code = in[5];
  e.location_source = in[6];
  e.context_kind = in[7];
  e.record_incarnation = getU64(in + 8);
  e.record_sequence = getU32(in + 16);
  e.transition_epoch_seconds = getU32(in + 20);
  e.occurrence_id = getU64(in + 24);
  e.latitude_e7 = getI32(in + 32);
  e.longitude_e7 = getI32(in + 36);
  e.location_age_seconds = getU16(in + 40);
  e.context_flags = getU16(in + 42);
  e.context_value = getU32(in + 44);
  const R valid = validateEventPlaintext(e);
  if (valid != R::kOk) return valid;
  *output = e;
  return R::kOk;
}

ProductSecureCodecResult encodeProductSecureFrame(const ProductSecureFrame& frame,
                                                  uint8_t* out,
                                                  size_t out_size) {
  if (out == nullptr) return R::kInvalidArgument;
  const R header = checkHeaderFields(frame);
  if (header != R::kOk) return header;
  const size_t ciphertext_size = frame.ciphertext_len;
  if (out_size != kProductSecureHeaderSize + ciphertext_size +
                      kProductSecureTagSize)
    return R::kBadLength;
  writeHeader(frame, out);
  memcpy(out + kProductSecureHeaderSize, frame.ciphertext, ciphertext_size);
  memcpy(out + kProductSecureHeaderSize + ciphertext_size, frame.tag,
         kProductSecureTagSize);
  return R::kOk;
}

ProductSecureCodecResult decodeProductSecureFrame(const uint8_t* in,
                                                  size_t in_size,
                                                  ProductSecureFrame* output) {
  if (in == nullptr || output == nullptr) return R::kInvalidArgument;
  // Section 19 order: exact supported length, version/type, reserved bytes,
  // context/family/profile, family length, epoch, counter. Nothing here is
  // authenticated; it only decides whether the frame is worth opening.
  if (in_size != kPeriodicFrameSize && in_size != kEventFrameSize)
    return R::kBadLength;
  if (in[0] != kProductSecureVersion || in[1] != kPacketTypeProductSecure)
    return R::kBadVersionOrType;
  if (in[7] != 0) return R::kBadReserved;
  if (in[2] != kProductSecurityContextDeviceD2a) return R::kBadContext;
  ProductSecureFrame frame;
  frame.app_family = in[3];
  frame.path_flags = in[4];
  frame.security_profile = in[5];
  frame.ciphertext_len = in[6];
  frame.device_id = getU64(in + 8);
  frame.key_epoch = getU32(in + 16);
  frame.security_counter = getU64(in + 20);
  const R header = checkHeaderFields(frame);
  if (header != R::kOk) return header;
  if (in_size != kProductSecureHeaderSize + frame.ciphertext_len +
                     kProductSecureTagSize)
    return R::kBadLength;
  memcpy(frame.ciphertext, in + kProductSecureHeaderSize, frame.ciphertext_len);
  memcpy(frame.tag, in + kProductSecureHeaderSize + frame.ciphertext_len,
         kProductSecureTagSize);
  *output = frame;
  return R::kOk;
}

ProductSecureCodecResult encodeProductSecureAad(
    const ProductSecureFrame& frame, uint8_t aad[kProductSecureHeaderSize]) {
  if (aad == nullptr) return R::kInvalidArgument;
  const R header = checkHeaderFields(frame);
  if (header != R::kOk) return header;
  writeHeader(frame, aad);
  return R::kOk;
}

void productSecureNonce(uint32_t key_epoch, uint64_t security_counter,
                        uint8_t nonce[kProductSecureNonceSize]) {
  putU32(nonce, key_epoch);
  nonce[4] = kProductTrafficDirectionD2a;
  putU64(nonce + 5, security_counter);
}

}  // namespace orun_tlp::tlp
