#pragma once

#include <stddef.h>
#include <stdint.h>

namespace orun_tlp::tlp {

// SF5B PRODUCT_SECURE codec: the frozen bytes of
// docs/architecture/ORUN_TLP_V2_PRODUCT_SECURE_WIRE.md sections 3-8 and the
// decoder rules of section 19.
//
// Codec only. It does not encrypt, does not own a security counter or record
// identity, and has no RF/runtime caller: before the explicit SF5F cutover the
// tracker still sends frozen TLP v1 POSITION (section 20). The AEAD itself is
// the existing M7P6 root D2A construction (section 3.2); this file only builds
// its exact AAD and nonce bytes.
//
// Codec structs are not wire serialization (section 19): every encoder writes
// the exact big-endian layout and every decoder leaves its output untouched on
// failure.
constexpr uint8_t kProductSecureVersion = 0x02U;
constexpr uint8_t kPacketTypeProductSecure = 0x04U;
constexpr uint8_t kProductSecurityContextDeviceD2a = 0x02U;
constexpr uint8_t kProductTrafficDirectionD2a = 0x01U;  // KDF/nonce byte
constexpr uint8_t kProductSecurityProfileRootAeadV1 = 0x01U;

constexpr uint8_t kProductAppFamilyPeriodic = 0x01U;
constexpr uint8_t kProductAppFamilyEvent = 0x02U;

constexpr uint8_t kProductPathFlagRelayAllowed = 1U << 0;
constexpr uint8_t kProductPathFlagCustodyRequested = 1U << 1;
constexpr uint8_t kProductPathFlagsAllowedMask = 0x03U;

constexpr size_t kProductSecureHeaderSize = 28U;
constexpr size_t kProductSecureTagSize = 8U;
constexpr size_t kProductSecureNonceSize = 13U;
constexpr size_t kPeriodicPlaintextSize = 64U;
constexpr size_t kEventPlaintextSize = 48U;
constexpr size_t kPeriodicFrameSize =
    kProductSecureHeaderSize + kPeriodicPlaintextSize + kProductSecureTagSize;
constexpr size_t kEventFrameSize =
    kProductSecureHeaderSize + kEventPlaintextSize + kProductSecureTagSize;
constexpr size_t kProductSecureMaxCiphertextSize = kPeriodicPlaintextSize;
constexpr size_t kProductSecureMaxFrameSize = kPeriodicFrameSize;
static_assert(kPeriodicFrameSize == 100U, "SF5B PERIODIC frame is 100 B");
static_assert(kEventFrameSize == 84U, "SF5B EVENT frame is 84 B");

// Section 6/7.2: shared location-source registry.
constexpr uint8_t kLocationSourceUnknown = 0U;
constexpr uint8_t kLocationSourceGnss = 1U;
constexpr uint8_t kLocationSourceFixed = 2U;
constexpr uint8_t kLocationSourcePhone = 3U;
constexpr uint8_t kLocationSourceManual = 4U;
constexpr uint16_t kLocationAgeUnknown = 0xFFFFU;
constexpr uint16_t kLocationAgeSaturated = 0xFFFEU;

// Section 7: PERIODIC_OBSERVATION schema 1.
constexpr uint8_t kPeriodicSchema = 0x01U;
constexpr uint8_t kPeriodicPeriodTimeValid = 1U << 0;
constexpr uint8_t kPeriodicLocationValid = 1U << 1;
constexpr uint8_t kPeriodicAltitudeValid = 1U << 2;
constexpr uint8_t kPeriodicLocationAgeValid = 1U << 3;
constexpr uint8_t kPeriodicActivityValid = 1U << 4;
constexpr uint8_t kPeriodicBatteryValid = 1U << 5;
constexpr uint8_t kPeriodicHealthValid = 1U << 6;
constexpr uint8_t kPeriodicRecordFlagsAllowedMask = 0x7FU;

constexpr uint8_t kActivityQualityUnknown = 0U;
constexpr uint8_t kActivityQualityPartial = 1U;
constexpr uint8_t kActivityQualityComplete = 2U;

// battery_state values equal BatteryState (battery_policy.h).
constexpr uint8_t kBatteryStateUnknown = 0U;
constexpr uint8_t kBatteryStateCritical = 3U;
constexpr uint8_t kBatteryQualityUnknown = 0U;
constexpr uint8_t kBatteryQualityMeasured = 1U;
constexpr uint8_t kBatteryQualityModelEstimated = 2U;

constexpr uint16_t kHealthGnssDegraded = 1U << 0;
constexpr uint16_t kHealthGnssFault = 1U << 1;
constexpr uint16_t kHealthSensorDegraded = 1U << 2;
constexpr uint16_t kHealthSensorFault = 1U << 3;
constexpr uint16_t kHealthRadioDegraded = 1U << 4;
constexpr uint16_t kHealthRadioFault = 1U << 5;
constexpr uint16_t kHealthStorageDegraded = 1U << 6;
constexpr uint16_t kHealthStorageFault = 1U << 7;
constexpr uint16_t kHealthSecurityDegraded = 1U << 8;
constexpr uint16_t kHealthSecurityFault = 1U << 9;
constexpr uint16_t kHealthTimeUntrusted = 1U << 10;
constexpr uint16_t kHealthFlagsAllowedMask = 0x07FFU;

// Section 8: EVENT schema 1.
constexpr uint8_t kEventSchema = 0x01U;
constexpr uint8_t kEventTypeGeofenceOutside = 0x01U;
constexpr uint8_t kEventTypeLost = 0x02U;
constexpr uint8_t kEventTypeBatteryState = 0x03U;
constexpr uint8_t kEventTypeSubsystemFault = 0x04U;
constexpr uint8_t kEventLifecycleActive = 0x01U;
constexpr uint8_t kEventLifecycleCleared = 0x02U;
constexpr uint8_t kEventSeverityInfo = 0x01U;
constexpr uint8_t kEventSeverityWarning = 0x02U;
constexpr uint8_t kEventSeverityCritical = 0x03U;
constexpr uint8_t kEventTransitionTimeValid = 1U << 0;
constexpr uint8_t kEventLocationValid = 1U << 1;
constexpr uint8_t kEventLocationAgeValid = 1U << 2;
constexpr uint8_t kEventContextValid = 1U << 3;
constexpr uint8_t kEventFlagsAllowedMask = 0x0FU;
constexpr uint8_t kEventContextNone = 0U;
constexpr uint8_t kEventContextBatteryMv = 1U;
constexpr uint8_t kEventContextSubsystemId = 2U;
constexpr uint8_t kBatteryReasonLow = 0x01U;
constexpr uint8_t kBatteryReasonCritical = 0x02U;

enum class ProductSecureCodecResult : uint8_t {
  kOk = 0,
  kInvalidArgument,
  kBadLength,
  kBadVersionOrType,
  kBadContext,
  kBadFamily,
  kBadProfile,
  kBadPathFlags,
  kBadReserved,
  kBadCiphertextLength,
  kBadEpoch,
  kBadCounter,
  kBadSchema,
  kBadFlags,
  kBadEnum,
  kBadIdentity,
  kBadTime,
  kBadLocation,
  kBadActivity,
  kBadBattery,
  kBadHealth,
  kBadContextValue,
};

const char* productSecureCodecResultName(ProductSecureCodecResult result);

// Header + opaque ciphertext + tag (section 3.1). The plaintext families are
// encoded separately and become `ciphertext` only through the AEAD owner.
struct ProductSecureFrame {
  uint8_t app_family = 0;
  uint8_t path_flags = 0;
  uint8_t security_profile = kProductSecurityProfileRootAeadV1;
  uint8_t ciphertext_len = 0;
  uint64_t device_id = 0;
  uint32_t key_epoch = 0;
  uint64_t security_counter = 0;
  uint8_t ciphertext[kProductSecureMaxCiphertextSize]{};
  uint8_t tag[kProductSecureTagSize]{};
};

struct PeriodicObservationPlaintext {
  uint8_t record_flags = 0;
  uint8_t location_source = kLocationSourceUnknown;
  uint8_t battery_state = kBatteryStateUnknown;
  uint64_t record_incarnation = 0;
  uint32_t record_sequence = 0;
  uint32_t period_end_epoch_seconds = 0;
  uint32_t period_duration_seconds = 0;
  int32_t latitude_e7 = 0;
  int32_t longitude_e7 = 0;
  int32_t altitude_mm = 0;
  uint16_t hdop_x100 = 0;
  uint16_t location_age_seconds = kLocationAgeUnknown;
  uint8_t satellites = 0;
  uint8_t activity_quality = kActivityQualityUnknown;
  uint8_t battery_quality = kBatteryQualityUnknown;
  uint32_t activity_coverage_seconds = 0;
  uint32_t active_seconds = 0;
  uint16_t activity_transition_count = 0;
  uint16_t movement_mean_abs_delta_mg = 0;
  uint16_t battery_mv = 0;
  uint16_t health_flags = 0;
  uint8_t gnss_failure_count = 0;
  uint8_t sensor_failure_count = 0;
  uint8_t storage_failure_count = 0;
  uint8_t radio_failure_count = 0;
};

struct EventPlaintext {
  uint8_t event_type = 0;
  uint8_t lifecycle_state = 0;
  uint8_t severity = 0;
  uint8_t event_flags = 0;
  uint8_t reason_code = 0;
  uint8_t location_source = kLocationSourceUnknown;
  uint8_t context_kind = kEventContextNone;
  uint64_t record_incarnation = 0;
  uint32_t record_sequence = 0;
  uint32_t transition_epoch_seconds = 0;
  uint64_t occurrence_id = 0;
  int32_t latitude_e7 = 0;
  int32_t longitude_e7 = 0;
  uint16_t location_age_seconds = kLocationAgeUnknown;
  uint16_t context_flags = 0;
  uint32_t context_value = 0;
};

// Schema-v1 semantic rules (sections 7 and 8). Encoders refuse what these
// refuse, so a producer can never emit non-canonical bytes.
ProductSecureCodecResult validatePeriodicPlaintext(
    const PeriodicObservationPlaintext& plaintext);
ProductSecureCodecResult validateEventPlaintext(const EventPlaintext& plaintext);

// `output_size` must be exactly the plaintext size.
ProductSecureCodecResult encodePeriodicPlaintext(
    const PeriodicObservationPlaintext& plaintext, uint8_t* output,
    size_t output_size);
ProductSecureCodecResult decodePeriodicPlaintext(
    const uint8_t* input, size_t input_size,
    PeriodicObservationPlaintext* plaintext);
ProductSecureCodecResult encodeEventPlaintext(const EventPlaintext& plaintext,
                                              uint8_t* output,
                                              size_t output_size);
ProductSecureCodecResult decodeEventPlaintext(const uint8_t* input,
                                              size_t input_size,
                                              EventPlaintext* plaintext);

// Pre-crypto frame rules (section 19). `output_size` must be exactly the
// family's frame size; `input_size` must be exactly the supported total.
ProductSecureCodecResult encodeProductSecureFrame(const ProductSecureFrame& frame,
                                                  uint8_t* output,
                                                  size_t output_size);
ProductSecureCodecResult decodeProductSecureFrame(const uint8_t* input,
                                                  size_t input_size,
                                                  ProductSecureFrame* frame);

// AEAD inputs for profile 0x01: AAD is exactly header bytes 0..27; the nonce
// is key_epoch_be32 || 0x01 (D2A) || security_counter_be64.
ProductSecureCodecResult encodeProductSecureAad(
    const ProductSecureFrame& frame, uint8_t aad[kProductSecureHeaderSize]);
void productSecureNonce(uint32_t key_epoch, uint64_t security_counter,
                        uint8_t nonce[kProductSecureNonceSize]);

}  // namespace orun_tlp::tlp
