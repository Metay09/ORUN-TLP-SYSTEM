#include "tlp_v2_history_secure.h"

#include <string.h>

namespace orun_tlp::tlp {
namespace {

constexpr int32_t kMinLatitudeE7 = -900000000;
constexpr int32_t kMaxLatitudeE7 = 900000000;
constexpr int32_t kMinLongitudeE7 = -1800000000;
constexpr int32_t kMaxLongitudeE7 = 1800000000;

bool coordinatesAreInRange(int32_t latitude_e7, int32_t longitude_e7) {
  return latitude_e7 >= kMinLatitudeE7 && latitude_e7 <= kMaxLatitudeE7 &&
         longitude_e7 >= kMinLongitudeE7 && longitude_e7 <= kMaxLongitudeE7;
}

void writeU16BigEndian(uint8_t* output, uint16_t value) {
  output[0] = static_cast<uint8_t>(value >> 8);
  output[1] = static_cast<uint8_t>(value);
}

void writeU32BigEndian(uint8_t* output, uint32_t value) {
  output[0] = static_cast<uint8_t>(value >> 24);
  output[1] = static_cast<uint8_t>(value >> 16);
  output[2] = static_cast<uint8_t>(value >> 8);
  output[3] = static_cast<uint8_t>(value);
}

void writeU64BigEndian(uint8_t* output, uint64_t value) {
  for (uint8_t index = 0; index < 8U; ++index)
    output[index] = static_cast<uint8_t>(value >> (56U - 8U * index));
}

uint16_t readU16BigEndian(const uint8_t* input) {
  return static_cast<uint16_t>((static_cast<uint16_t>(input[0]) << 8) |
                               input[1]);
}

uint32_t readU32BigEndian(const uint8_t* input) {
  return (static_cast<uint32_t>(input[0]) << 24) |
         (static_cast<uint32_t>(input[1]) << 16) |
         (static_cast<uint32_t>(input[2]) << 8) |
         static_cast<uint32_t>(input[3]);
}

uint64_t readU64BigEndian(const uint8_t* input) {
  uint64_t value = 0;
  for (uint8_t index = 0; index < 8U; ++index)
    value = (value << 8U) | input[index];
  return value;
}

bool securityContextValid(uint8_t security_context) {
  return security_context == kHistorySecurityContextDeviceD2a ||
         security_context == kHistorySecurityContextBackendA2d;
}

bool appFamilyValid(uint8_t app_family) {
  return app_family == kHistoryAppFamilyObservation ||
         app_family == kHistoryAppFamilyBackendDurableReceipt;
}

bool ciphertextLengthValid(uint8_t app_family, uint8_t ciphertext_len) {
  if (app_family == kHistoryAppFamilyObservation)
    return ciphertext_len == kHistoryObservationPlaintextSize;

  if (app_family != kHistoryAppFamilyBackendDurableReceipt ||
      ciphertext_len < kHistoryReceiptFixedPlaintextSize + sizeof(uint64_t) ||
      ciphertext_len > kHistoryReceiptMaxPlaintextSize) {
    return false;
  }

  return (ciphertext_len - kHistoryReceiptFixedPlaintextSize) %
             sizeof(uint64_t) ==
         0U;
}

bool observationValid(const HistoryObservationPlaintext& observation) {
  if (observation.history_record_identity == 0U)
    return false;
  if (!coordinatesAreInRange(observation.latitude_e7,
                             observation.longitude_e7))
    return false;
  if ((observation.position_flags &
       static_cast<uint8_t>(~kHistoryPositionFlagsAllowedMask)) != 0U)
    return false;
  if ((observation.position_flags & kHistoryPositionFlagValidFix) == 0U)
    return false;
  if ((observation.position_flags & kHistoryPositionFlagValidUtcTime) == 0U &&
      observation.gnss_utc_epoch_seconds != 0U)
    return false;
  return true;
}

bool receiptValid(const BackendDurableReceiptPlaintext& receipt) {
  if (receipt.count == 0U || receipt.count > kHistoryReceiptMaxIdentities)
    return false;

  uint64_t previous = 0;
  for (uint8_t index = 0; index < receipt.count; ++index) {
    const uint64_t identity = receipt.history_record_identities[index];
    if (identity == 0U || (index != 0U && identity <= previous))
      return false;
    previous = identity;
  }
  return true;
}

}  // namespace

bool historySecureContextFamilyAllowed(uint8_t security_context,
                                       uint8_t app_family) {
  return (security_context == kHistorySecurityContextDeviceD2a &&
          app_family == kHistoryAppFamilyObservation) ||
         (security_context == kHistorySecurityContextBackendA2d &&
          app_family == kHistoryAppFamilyBackendDurableReceipt);
}

bool historyTrafficDirectionForSecurityContext(uint8_t security_context,
                                               uint8_t* direction) {
  if (direction == nullptr)
    return false;
  if (security_context == kHistorySecurityContextDeviceD2a) {
    *direction = kHistoryTrafficDirectionD2a;
    return true;
  }
  if (security_context == kHistorySecurityContextBackendA2d) {
    *direction = kHistoryTrafficDirectionA2d;
    return true;
  }
  return false;
}

bool validateHistorySecurePacket(const HistorySecurePacket& packet) {
  return securityContextValid(packet.security_context) &&
         appFamilyValid(packet.app_family) &&
         historySecureContextFamilyAllowed(packet.security_context,
                                           packet.app_family) &&
         (packet.path_flags &
          static_cast<uint8_t>(~kHistoryPathFlagsAllowedMask)) == 0U &&
         ciphertextLengthValid(packet.app_family, packet.ciphertext_len) &&
         packet.key_epoch != UINT32_MAX &&
         packet.security_counter != 0U &&
         packet.history_incarnation != 0U;
}

bool serializeHistorySecurePacket(const HistorySecurePacket& packet,
                                  uint8_t* output, size_t output_size) {
  const size_t expected_size =
      kHistorySecureHeaderSize + packet.ciphertext_len + kHistorySecureTagSize;
  if (output == nullptr || output_size != expected_size ||
      !validateHistorySecurePacket(packet))
    return false;

  output[0] = kHistorySecureProtocolVersion;
  output[1] = kPacketTypeHistorySecure;
  output[2] = packet.security_context;
  output[3] = packet.app_family;
  output[4] = packet.path_flags;
  output[5] = packet.ciphertext_len;
  output[6] = 0U;
  output[7] = 0U;
  writeU64BigEndian(&output[8], packet.device_id);
  writeU32BigEndian(&output[16], packet.key_epoch);
  writeU64BigEndian(&output[20], packet.security_counter);
  writeU64BigEndian(&output[28], packet.history_incarnation);
  memcpy(&output[kHistorySecureHeaderSize], packet.ciphertext,
         packet.ciphertext_len);
  memcpy(&output[kHistorySecureHeaderSize + packet.ciphertext_len],
         packet.tag, kHistorySecureTagSize);
  return true;
}

HistorySecureDecodeStatus deserializeHistorySecurePacket(
    const uint8_t* input, size_t input_size, HistorySecurePacket* packet) {
  if (input == nullptr || packet == nullptr ||
      input_size < kHistorySecureMinPacketSize ||
      input_size > kHistorySecureMaxPacketSize)
    return HistorySecureDecodeStatus::kLength;
  if (input[0] != kHistorySecureProtocolVersion)
    return HistorySecureDecodeStatus::kVersion;
  if (input[1] != kPacketTypeHistorySecure)
    return HistorySecureDecodeStatus::kType;
  if (!securityContextValid(input[2]))
    return HistorySecureDecodeStatus::kSecurityContext;
  if (!appFamilyValid(input[3]))
    return HistorySecureDecodeStatus::kAppFamily;
  if (!historySecureContextFamilyAllowed(input[2], input[3]))
    return HistorySecureDecodeStatus::kContextFamily;
  if ((input[4] & static_cast<uint8_t>(~kHistoryPathFlagsAllowedMask)) != 0U)
    return HistorySecureDecodeStatus::kPathFlags;
  if (!ciphertextLengthValid(input[3], input[5]))
    return HistorySecureDecodeStatus::kCiphertextLength;
  if (input[6] != 0U || input[7] != 0U)
    return HistorySecureDecodeStatus::kReserved;

  const size_t expected_size =
      kHistorySecureHeaderSize + input[5] + kHistorySecureTagSize;
  if (input_size != expected_size)
    return HistorySecureDecodeStatus::kLength;

  const uint32_t key_epoch = readU32BigEndian(&input[16]);
  const uint64_t security_counter = readU64BigEndian(&input[20]);
  const uint64_t history_incarnation = readU64BigEndian(&input[28]);
  if (key_epoch == UINT32_MAX)
    return HistorySecureDecodeStatus::kKeyEpoch;
  if (security_counter == 0U)
    return HistorySecureDecodeStatus::kSecurityCounter;
  if (history_incarnation == 0U)
    return HistorySecureDecodeStatus::kHistoryIncarnation;

  HistorySecurePacket decoded{};
  decoded.security_context = input[2];
  decoded.app_family = input[3];
  decoded.path_flags = input[4];
  decoded.ciphertext_len = input[5];
  decoded.device_id = readU64BigEndian(&input[8]);
  decoded.key_epoch = key_epoch;
  decoded.security_counter = security_counter;
  decoded.history_incarnation = history_incarnation;
  memcpy(decoded.ciphertext, &input[kHistorySecureHeaderSize],
         decoded.ciphertext_len);
  memcpy(decoded.tag,
         &input[kHistorySecureHeaderSize + decoded.ciphertext_len],
         kHistorySecureTagSize);
  *packet = decoded;
  return HistorySecureDecodeStatus::kOk;
}

bool serializeHistoryObservationPlaintext(
    const HistoryObservationPlaintext& observation,
    uint8_t* output, size_t output_size) {
  if (output == nullptr || output_size != kHistoryObservationPlaintextSize ||
      !observationValid(observation))
    return false;

  output[0] = kHistoryObservationSchema;
  writeU64BigEndian(&output[1], observation.history_record_identity);
  writeU32BigEndian(&output[9], observation.gnss_utc_epoch_seconds);
  writeU32BigEndian(&output[13],
                    static_cast<uint32_t>(observation.latitude_e7));
  writeU32BigEndian(&output[17],
                    static_cast<uint32_t>(observation.longitude_e7));
  writeU32BigEndian(&output[21],
                    static_cast<uint32_t>(observation.altitude_mm));
  writeU16BigEndian(&output[25], observation.hdop_x100);
  output[27] = observation.satellites;
  output[28] = observation.position_flags;
  return true;
}

HistoryPlaintextDecodeStatus deserializeHistoryObservationPlaintext(
    const uint8_t* input, size_t input_size,
    HistoryObservationPlaintext* observation) {
  if (input == nullptr || observation == nullptr ||
      input_size != kHistoryObservationPlaintextSize)
    return HistoryPlaintextDecodeStatus::kLength;
  if (input[0] != kHistoryObservationSchema)
    return HistoryPlaintextDecodeStatus::kSchema;

  HistoryObservationPlaintext decoded{};
  decoded.history_record_identity = readU64BigEndian(&input[1]);
  decoded.gnss_utc_epoch_seconds = readU32BigEndian(&input[9]);
  decoded.latitude_e7 =
      static_cast<int32_t>(readU32BigEndian(&input[13]));
  decoded.longitude_e7 =
      static_cast<int32_t>(readU32BigEndian(&input[17]));
  decoded.altitude_mm =
      static_cast<int32_t>(readU32BigEndian(&input[21]));
  decoded.hdop_x100 = readU16BigEndian(&input[25]);
  decoded.satellites = input[27];
  decoded.position_flags = input[28];

  if (decoded.history_record_identity == 0U)
    return HistoryPlaintextDecodeStatus::kIdentity;
  if (!coordinatesAreInRange(decoded.latitude_e7, decoded.longitude_e7))
    return HistoryPlaintextDecodeStatus::kPositionCoordinates;
  if ((decoded.position_flags &
       static_cast<uint8_t>(~kHistoryPositionFlagsAllowedMask)) != 0U)
    return HistoryPlaintextDecodeStatus::kPositionFlags;
  if ((decoded.position_flags & kHistoryPositionFlagValidFix) == 0U)
    return HistoryPlaintextDecodeStatus::kPositionFix;
  if ((decoded.position_flags & kHistoryPositionFlagValidUtcTime) == 0U &&
      decoded.gnss_utc_epoch_seconds != 0U)
    return HistoryPlaintextDecodeStatus::kPositionTime;

  *observation = decoded;
  return HistoryPlaintextDecodeStatus::kOk;
}

size_t backendDurableReceiptPlaintextSize(uint8_t count) {
  if (count == 0U || count > kHistoryReceiptMaxIdentities)
    return 0U;
  return kHistoryReceiptFixedPlaintextSize +
         static_cast<size_t>(count) * sizeof(uint64_t);
}

bool serializeBackendDurableReceiptPlaintext(
    const BackendDurableReceiptPlaintext& receipt,
    uint8_t* output, size_t output_size) {
  const size_t expected_size = backendDurableReceiptPlaintextSize(receipt.count);
  if (output == nullptr || expected_size == 0U ||
      output_size != expected_size || !receiptValid(receipt))
    return false;

  output[0] = kHistoryReceiptSchema;
  output[1] = kHistoryReceiptScopeBackendDurable;
  output[2] = receipt.count;
  output[3] = 0U;
  for (uint8_t index = 0; index < receipt.count; ++index)
    writeU64BigEndian(&output[4U + 8U * index],
                      receipt.history_record_identities[index]);
  return true;
}

HistoryPlaintextDecodeStatus deserializeBackendDurableReceiptPlaintext(
    const uint8_t* input, size_t input_size,
    BackendDurableReceiptPlaintext* receipt) {
  if (input == nullptr || receipt == nullptr ||
      input_size < kHistoryReceiptFixedPlaintextSize + sizeof(uint64_t) ||
      input_size > kHistoryReceiptMaxPlaintextSize)
    return HistoryPlaintextDecodeStatus::kLength;
  if (input[0] != kHistoryReceiptSchema)
    return HistoryPlaintextDecodeStatus::kSchema;
  if (input[1] != kHistoryReceiptScopeBackendDurable)
    return HistoryPlaintextDecodeStatus::kScope;
  if (input[3] != 0U)
    return HistoryPlaintextDecodeStatus::kReserved;

  const uint8_t count = input[2];
  const size_t expected_size = backendDurableReceiptPlaintextSize(count);
  if (expected_size == 0U)
    return HistoryPlaintextDecodeStatus::kCount;
  if (input_size != expected_size)
    return HistoryPlaintextDecodeStatus::kLength;

  BackendDurableReceiptPlaintext decoded{};
  decoded.count = count;
  uint64_t previous = 0;
  for (uint8_t index = 0; index < count; ++index) {
    const uint64_t identity = readU64BigEndian(&input[4U + 8U * index]);
    if (identity == 0U)
      return HistoryPlaintextDecodeStatus::kIdentity;
    if (index != 0U && identity <= previous)
      return HistoryPlaintextDecodeStatus::kIdentityOrder;
    decoded.history_record_identities[index] = identity;
    previous = identity;
  }

  *receipt = decoded;
  return HistoryPlaintextDecodeStatus::kOk;
}

}  // namespace orun_tlp::tlp
