#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "tlp_v2_history_secure.h"

using namespace orun_tlp::tlp;

namespace {

void writeU32BigEndian(uint8_t* output, uint32_t value) {
  output[0] = static_cast<uint8_t>(value >> 24);
  output[1] = static_cast<uint8_t>(value >> 16);
  output[2] = static_cast<uint8_t>(value >> 8);
  output[3] = static_cast<uint8_t>(value);
}

void contextDirectionRegistry() {
  static_assert(kHistorySecurityContextBackendA2d == 0x01U);
  static_assert(kHistorySecurityContextDeviceD2a == 0x02U);
  static_assert(kHistoryTrafficDirectionD2a == 0x01U);
  static_assert(kHistoryTrafficDirectionA2d == 0x02U);

  uint8_t direction = 0;
  assert(historyTrafficDirectionForSecurityContext(
      kHistorySecurityContextDeviceD2a, &direction));
  assert(direction == kHistoryTrafficDirectionD2a);
  assert(historyTrafficDirectionForSecurityContext(
      kHistorySecurityContextBackendA2d, &direction));
  assert(direction == kHistoryTrafficDirectionA2d);
  assert(!historyTrafficDirectionForSecurityContext(0x00U, &direction));
  assert(!historyTrafficDirectionForSecurityContext(
      kHistorySecurityContextDeviceD2a, nullptr));
}

HistoryObservationPlaintext goldenObservation() {
  HistoryObservationPlaintext observation{};
  observation.history_record_identity = 0x0102030405060708ULL;
  observation.gnss_utc_epoch_seconds = 0x11223344U;
  observation.latitude_e7 = 0x01020304;
  observation.longitude_e7 = -1;
  observation.altitude_mm = 0x05060708;
  observation.hdop_x100 = 0x1234U;
  observation.satellites = 8U;
  observation.position_flags =
      kHistoryPositionFlagValidFix |
      kHistoryPositionFlagValidUtcTime |
      kHistoryPositionFlag3dFix;
  return observation;
}

void observationPlaintextGolden() {
  const HistoryObservationPlaintext observation = goldenObservation();

  const uint8_t expected[kHistoryObservationPlaintextSize] = {
      0x01,
      0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,
      0x11,0x22,0x33,0x44,
      0x01,0x02,0x03,0x04,
      0xFF,0xFF,0xFF,0xFF,
      0x05,0x06,0x07,0x08,
      0x12,0x34,0x08,0x07};

  uint8_t bytes[kHistoryObservationPlaintextSize]{};
  assert(serializeHistoryObservationPlaintext(observation, bytes,
                                              sizeof(bytes)));
  assert(memcmp(bytes, expected, sizeof(expected)) == 0);

  HistoryObservationPlaintext decoded{};
  assert(deserializeHistoryObservationPlaintext(
             bytes, sizeof(bytes), &decoded) ==
         HistoryPlaintextDecodeStatus::kOk);
  assert(decoded.history_record_identity ==
         observation.history_record_identity);
  assert(decoded.gnss_utc_epoch_seconds ==
         observation.gnss_utc_epoch_seconds);
  assert(decoded.latitude_e7 == observation.latitude_e7);
  assert(decoded.longitude_e7 == observation.longitude_e7);
  assert(decoded.altitude_mm == observation.altitude_mm);
  assert(decoded.hdop_x100 == observation.hdop_x100);
  assert(decoded.satellites == observation.satellites);
  assert(decoded.position_flags == observation.position_flags);
}

void observationCoordinateBounds() {
  HistoryObservationPlaintext observation = goldenObservation();
  observation.latitude_e7 = -900000000;
  observation.longitude_e7 = -1800000000;
  uint8_t bytes[kHistoryObservationPlaintextSize]{};
  assert(serializeHistoryObservationPlaintext(observation, bytes,
                                              sizeof(bytes)));

  observation.latitude_e7 = 900000000;
  observation.longitude_e7 = 1800000000;
  assert(serializeHistoryObservationPlaintext(observation, bytes,
                                              sizeof(bytes)));

  observation.latitude_e7 = -900000001;
  assert(!serializeHistoryObservationPlaintext(observation, bytes,
                                               sizeof(bytes)));
  observation.latitude_e7 = 900000001;
  assert(!serializeHistoryObservationPlaintext(observation, bytes,
                                               sizeof(bytes)));

  observation = goldenObservation();
  observation.longitude_e7 = -1800000001;
  assert(!serializeHistoryObservationPlaintext(observation, bytes,
                                               sizeof(bytes)));
  observation.longitude_e7 = 1800000001;
  assert(!serializeHistoryObservationPlaintext(observation, bytes,
                                               sizeof(bytes)));

  observation = goldenObservation();
  assert(serializeHistoryObservationPlaintext(observation, bytes,
                                              sizeof(bytes)));
  writeU32BigEndian(&bytes[13], static_cast<uint32_t>(900000001));
  HistoryObservationPlaintext decoded{};
  assert(deserializeHistoryObservationPlaintext(
             bytes, sizeof(bytes), &decoded) ==
         HistoryPlaintextDecodeStatus::kPositionCoordinates);

  assert(serializeHistoryObservationPlaintext(observation, bytes,
                                              sizeof(bytes)));
  writeU32BigEndian(&bytes[17], static_cast<uint32_t>(1800000001));
  assert(deserializeHistoryObservationPlaintext(
             bytes, sizeof(bytes), &decoded) ==
         HistoryPlaintextDecodeStatus::kPositionCoordinates);
}

BackendDurableReceiptPlaintext goldenReceipt() {
  BackendDurableReceiptPlaintext receipt{};
  receipt.count = 2U;
  receipt.history_record_identities[0] = 0x0102030405060708ULL;
  receipt.history_record_identities[1] = 0x1112131415161718ULL;
  return receipt;
}

void receiptPlaintextGoldenAndBounds() {
  BackendDurableReceiptPlaintext receipt = goldenReceipt();

  const uint8_t expected[20] = {
      0x01,0x01,0x02,0x00,
      0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,
      0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18};

  uint8_t bytes[20]{};
  assert(backendDurableReceiptPlaintextSize(receipt.count) == sizeof(bytes));
  assert(serializeBackendDurableReceiptPlaintext(receipt, bytes,
                                                 sizeof(bytes)));
  assert(memcmp(bytes, expected, sizeof(expected)) == 0);

  BackendDurableReceiptPlaintext decoded{};
  assert(deserializeBackendDurableReceiptPlaintext(
             bytes, sizeof(bytes), &decoded) ==
         HistoryPlaintextDecodeStatus::kOk);
  assert(decoded.count == 2U);
  assert(decoded.history_record_identities[0] ==
         receipt.history_record_identities[0]);
  assert(decoded.history_record_identities[1] ==
         receipt.history_record_identities[1]);

  BackendDurableReceiptPlaintext max_receipt{};
  max_receipt.count = kHistoryReceiptMaxIdentities;
  for (uint8_t i = 0; i < max_receipt.count; ++i)
    max_receipt.history_record_identities[i] = 100U + i;
  uint8_t max_bytes[kHistoryReceiptMaxPlaintextSize]{};
  assert(serializeBackendDurableReceiptPlaintext(
      max_receipt, max_bytes, sizeof(max_bytes)));

  receipt.history_record_identities[1] =
      receipt.history_record_identities[0];
  assert(!serializeBackendDurableReceiptPlaintext(receipt, bytes,
                                                  sizeof(bytes)));
  receipt.count = 0U;
  assert(!serializeBackendDurableReceiptPlaintext(receipt, bytes,
                                                  sizeof(bytes)));
}

HistorySecurePacket observationVectorEnvelope() {
  HistorySecurePacket packet{};
  packet.security_context = kHistorySecurityContextDeviceD2a;
  packet.app_family = kHistoryAppFamilyObservation;
  packet.path_flags = kHistoryPathFlagRelayAllowed;
  packet.ciphertext_len = kHistoryObservationPlaintextSize;
  packet.device_id = 0x1122334455667788ULL;
  packet.key_epoch = 0x01020304U;
  packet.security_counter = 0x1122334455667788ULL;
  packet.history_incarnation = 0xA1A2A3A4A5A6A7A8ULL;
  const uint8_t ciphertext[kHistoryObservationPlaintextSize] = {
      0x61,0xFF,0x0E,0xC3,0xE2,0x11,0xAE,0x14,
      0x3C,0xBE,0xE9,0x2D,0x89,0x5C,0xB1,0x63,
      0x11,0x34,0xAA,0x08,0xFD,0x95,0x7B,0x0B,
      0x96,0x09,0x3F,0x0D,0x66};
  const uint8_t tag[kHistorySecureTagSize] = {
      0x12,0xC1,0x8C,0x91,0xAE,0xB7,0x75,0x0A};
  memcpy(packet.ciphertext, ciphertext, sizeof(ciphertext));
  memcpy(packet.tag, tag, sizeof(tag));
  return packet;
}

void observationEnvelopeSecurityVectorGolden() {
  const HistorySecurePacket packet = observationVectorEnvelope();
  uint8_t bytes[kHistoryObservationPacketSize]{};
  assert(serializeHistorySecurePacket(packet, bytes, sizeof(bytes)));

  const uint8_t expected[kHistoryObservationPacketSize] = {
      0x02,0x03,0x02,0x01,0x01,0x1D,0x00,0x00,
      0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,
      0x01,0x02,0x03,0x04,0x11,0x22,0x33,0x44,
      0x55,0x66,0x77,0x88,0xA1,0xA2,0xA3,0xA4,
      0xA5,0xA6,0xA7,0xA8,0x61,0xFF,0x0E,0xC3,
      0xE2,0x11,0xAE,0x14,0x3C,0xBE,0xE9,0x2D,
      0x89,0x5C,0xB1,0x63,0x11,0x34,0xAA,0x08,
      0xFD,0x95,0x7B,0x0B,0x96,0x09,0x3F,0x0D,
      0x66,0x12,0xC1,0x8C,0x91,0xAE,0xB7,0x75,
      0x0A};
  assert(memcmp(bytes, expected, sizeof(expected)) == 0);

  HistorySecurePacket decoded{};
  assert(deserializeHistorySecurePacket(bytes, sizeof(bytes), &decoded) ==
         HistorySecureDecodeStatus::kOk);
  assert(decoded.security_context == packet.security_context);
  assert(decoded.app_family == packet.app_family);
  assert(decoded.path_flags == packet.path_flags);
  assert(decoded.ciphertext_len == packet.ciphertext_len);
  assert(decoded.device_id == packet.device_id);
  assert(decoded.key_epoch == packet.key_epoch);
  assert(decoded.security_counter == packet.security_counter);
  assert(decoded.history_incarnation == packet.history_incarnation);
  assert(memcmp(decoded.ciphertext, packet.ciphertext,
                packet.ciphertext_len) == 0);
  assert(memcmp(decoded.tag, packet.tag, kHistorySecureTagSize) == 0);
}

void receiptEnvelopeSecurityVectorGolden() {
  HistorySecurePacket packet{};
  packet.security_context = kHistorySecurityContextBackendA2d;
  packet.app_family = kHistoryAppFamilyBackendDurableReceipt;
  packet.path_flags = kHistoryPathFlagRelayAllowed;
  packet.ciphertext_len = 20U;
  packet.device_id = 0x1122334455667788ULL;
  packet.key_epoch = 0x01020304U;
  packet.security_counter = 0x8877665544332211ULL;
  packet.history_incarnation = 0xA1A2A3A4A5A6A7A8ULL;
  const uint8_t ciphertext[20] = {
      0x19,0x77,0xAF,0xF6,0x9F,0x4A,0x3E,0xC1,
      0xEF,0x72,0xBF,0xD8,0x19,0x05,0x86,0x31,
      0x14,0x0B,0xF1,0x6A};
  const uint8_t tag[kHistorySecureTagSize] = {
      0xDC,0x88,0xD6,0xA5,0xFD,0x0A,0xE9,0x62};
  memcpy(packet.ciphertext, ciphertext, sizeof(ciphertext));
  memcpy(packet.tag, tag, sizeof(tag));

  uint8_t bytes[64]{};
  assert(serializeHistorySecurePacket(packet, bytes, sizeof(bytes)));

  const uint8_t expected[64] = {
      0x02,0x03,0x01,0x02,0x01,0x14,0x00,0x00,
      0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,
      0x01,0x02,0x03,0x04,0x88,0x77,0x66,0x55,
      0x44,0x33,0x22,0x11,0xA1,0xA2,0xA3,0xA4,
      0xA5,0xA6,0xA7,0xA8,0x19,0x77,0xAF,0xF6,
      0x9F,0x4A,0x3E,0xC1,0xEF,0x72,0xBF,0xD8,
      0x19,0x05,0x86,0x31,0x14,0x0B,0xF1,0x6A,
      0xDC,0x88,0xD6,0xA5,0xFD,0x0A,0xE9,0x62};
  assert(memcmp(bytes, expected, sizeof(expected)) == 0);

  HistorySecurePacket decoded{};
  assert(deserializeHistorySecurePacket(bytes, sizeof(bytes), &decoded) ==
         HistorySecureDecodeStatus::kOk);
}

void minAndMaxReceiptEnvelope() {
  HistorySecurePacket packet{};
  packet.security_context = kHistorySecurityContextBackendA2d;
  packet.app_family = kHistoryAppFamilyBackendDurableReceipt;
  packet.path_flags = 0U;
  packet.ciphertext_len =
      kHistoryReceiptFixedPlaintextSize + sizeof(uint64_t);
  packet.device_id = 0x1122334455667788ULL;
  packet.key_epoch = 1U;
  packet.security_counter = 1U;
  packet.history_incarnation = 1U;

  uint8_t min_bytes[kHistoryReceiptMinPacketSize]{};
  assert(sizeof(min_bytes) == 56U);
  assert(serializeHistorySecurePacket(packet, min_bytes, sizeof(min_bytes)));

  HistorySecurePacket decoded{};
  assert(deserializeHistorySecurePacket(
             min_bytes, sizeof(min_bytes), &decoded) ==
         HistorySecureDecodeStatus::kOk);

  packet.path_flags = kHistoryPathFlagRelayAllowed;
  packet.ciphertext_len = kHistoryReceiptMaxPlaintextSize;
  packet.key_epoch = 7U;
  packet.security_counter = 9U;
  packet.history_incarnation = 11U;
  uint8_t max_bytes[kHistorySecureMaxPacketSize]{};
  assert(sizeof(max_bytes) == 96U);
  assert(serializeHistorySecurePacket(packet, max_bytes, sizeof(max_bytes)));
  assert(deserializeHistorySecurePacket(
             max_bytes, sizeof(max_bytes), &decoded) ==
         HistorySecureDecodeStatus::kOk);
}

void malformedEnvelope() {
  const HistorySecurePacket packet = observationVectorEnvelope();
  uint8_t bytes[kHistoryObservationPacketSize]{};
  assert(serializeHistorySecurePacket(packet, bytes, sizeof(bytes)));

  uint8_t bad[kHistoryObservationPacketSize]{};
  HistorySecurePacket decoded{};

  memcpy(bad, bytes, sizeof(bad));
  bad[0] = 0x01U;
  assert(deserializeHistorySecurePacket(bad, sizeof(bad), &decoded) ==
         HistorySecureDecodeStatus::kVersion);

  memcpy(bad, bytes, sizeof(bad));
  bad[1] = 0x01U;  // DELEGATED_SECURE_APP type is not HISTORY_SECURE.
  assert(deserializeHistorySecurePacket(bad, sizeof(bad), &decoded) ==
         HistorySecureDecodeStatus::kType);

  memcpy(bad, bytes, sizeof(bad));
  bad[2] = 0x00U;
  assert(deserializeHistorySecurePacket(bad, sizeof(bad), &decoded) ==
         HistorySecureDecodeStatus::kSecurityContext);

  memcpy(bad, bytes, sizeof(bad));
  bad[2] = 0x03U;
  assert(deserializeHistorySecurePacket(bad, sizeof(bad), &decoded) ==
         HistorySecureDecodeStatus::kSecurityContext);

  memcpy(bad, bytes, sizeof(bad));
  bad[3] = 0x7FU;
  assert(deserializeHistorySecurePacket(bad, sizeof(bad), &decoded) ==
         HistorySecureDecodeStatus::kAppFamily);

  memcpy(bad, bytes, sizeof(bad));
  bad[2] = kHistorySecurityContextBackendA2d;
  assert(deserializeHistorySecurePacket(bad, sizeof(bad), &decoded) ==
         HistorySecureDecodeStatus::kContextFamily);

  memcpy(bad, bytes, sizeof(bad));
  bad[4] = 0x80U;
  assert(deserializeHistorySecurePacket(bad, sizeof(bad), &decoded) ==
         HistorySecureDecodeStatus::kPathFlags);

  memcpy(bad, bytes, sizeof(bad));
  bad[5] = 28U;
  assert(deserializeHistorySecurePacket(bad, sizeof(bad), &decoded) ==
         HistorySecureDecodeStatus::kCiphertextLength);

  memcpy(bad, bytes, sizeof(bad));
  bad[6] = 1U;
  assert(deserializeHistorySecurePacket(bad, sizeof(bad), &decoded) ==
         HistorySecureDecodeStatus::kReserved);

  memcpy(bad, bytes, sizeof(bad));
  bad[7] = 1U;
  assert(deserializeHistorySecurePacket(bad, sizeof(bad), &decoded) ==
         HistorySecureDecodeStatus::kReserved);

  memcpy(bad, bytes, sizeof(bad));
  memset(&bad[16], 0xFF, 4U);
  assert(deserializeHistorySecurePacket(bad, sizeof(bad), &decoded) ==
         HistorySecureDecodeStatus::kKeyEpoch);

  memcpy(bad, bytes, sizeof(bad));
  memset(&bad[20], 0, 8U);
  assert(deserializeHistorySecurePacket(bad, sizeof(bad), &decoded) ==
         HistorySecureDecodeStatus::kSecurityCounter);

  memcpy(bad, bytes, sizeof(bad));
  memset(&bad[28], 0, 8U);
  assert(deserializeHistorySecurePacket(bad, sizeof(bad), &decoded) ==
         HistorySecureDecodeStatus::kHistoryIncarnation);

  assert(deserializeHistorySecurePacket(
             bytes, kHistorySecureMinPacketSize - 1U, &decoded) ==
         HistorySecureDecodeStatus::kLength);

  uint8_t oversized[kHistorySecureMaxPacketSize + 1U]{};
  assert(deserializeHistorySecurePacket(
             oversized, sizeof(oversized), &decoded) ==
         HistorySecureDecodeStatus::kLength);

  assert(deserializeHistorySecurePacket(nullptr, sizeof(bytes), &decoded) ==
         HistorySecureDecodeStatus::kLength);
  assert(deserializeHistorySecurePacket(bytes, sizeof(bytes), nullptr) ==
         HistorySecureDecodeStatus::kLength);
}

void malformedObservationPlaintext() {
  HistoryObservationPlaintext observation = goldenObservation();
  uint8_t bytes[kHistoryObservationPlaintextSize]{};
  assert(serializeHistoryObservationPlaintext(observation, bytes,
                                              sizeof(bytes)));
  HistoryObservationPlaintext decoded{};

  uint8_t bad[kHistoryObservationPlaintextSize]{};

  memcpy(bad, bytes, sizeof(bad));
  bad[0] = 2U;
  assert(deserializeHistoryObservationPlaintext(
             bad, sizeof(bad), &decoded) ==
         HistoryPlaintextDecodeStatus::kSchema);

  memcpy(bad, bytes, sizeof(bad));
  memset(&bad[1], 0, 8U);
  assert(deserializeHistoryObservationPlaintext(
             bad, sizeof(bad), &decoded) ==
         HistoryPlaintextDecodeStatus::kIdentity);

  memcpy(bad, bytes, sizeof(bad));
  bad[28] = 0x80U;
  assert(deserializeHistoryObservationPlaintext(
             bad, sizeof(bad), &decoded) ==
         HistoryPlaintextDecodeStatus::kPositionFlags);

  memcpy(bad, bytes, sizeof(bad));
  bad[28] = kHistoryPositionFlagValidUtcTime;
  assert(deserializeHistoryObservationPlaintext(
             bad, sizeof(bad), &decoded) ==
         HistoryPlaintextDecodeStatus::kPositionFix);

  memcpy(bad, bytes, sizeof(bad));
  bad[28] = kHistoryPositionFlagValidFix;
  assert(deserializeHistoryObservationPlaintext(
             bad, sizeof(bad), &decoded) ==
         HistoryPlaintextDecodeStatus::kPositionTime);

  assert(deserializeHistoryObservationPlaintext(
             bytes, sizeof(bytes) - 1U, &decoded) ==
         HistoryPlaintextDecodeStatus::kLength);
  assert(deserializeHistoryObservationPlaintext(
             nullptr, sizeof(bytes), &decoded) ==
         HistoryPlaintextDecodeStatus::kLength);
  assert(deserializeHistoryObservationPlaintext(
             bytes, sizeof(bytes), nullptr) ==
         HistoryPlaintextDecodeStatus::kLength);
}

void malformedReceiptPlaintext() {
  BackendDurableReceiptPlaintext receipt = goldenReceipt();
  uint8_t bytes[20]{};
  assert(serializeBackendDurableReceiptPlaintext(receipt, bytes,
                                                 sizeof(bytes)));
  BackendDurableReceiptPlaintext decoded{};

  uint8_t bad[20]{};

  memcpy(bad, bytes, sizeof(bad));
  bad[0] = 2U;
  assert(deserializeBackendDurableReceiptPlaintext(
             bad, sizeof(bad), &decoded) ==
         HistoryPlaintextDecodeStatus::kSchema);

  memcpy(bad, bytes, sizeof(bad));
  bad[1] = 2U;
  assert(deserializeBackendDurableReceiptPlaintext(
             bad, sizeof(bad), &decoded) ==
         HistoryPlaintextDecodeStatus::kScope);

  memcpy(bad, bytes, sizeof(bad));
  bad[3] = 1U;
  assert(deserializeBackendDurableReceiptPlaintext(
             bad, sizeof(bad), &decoded) ==
         HistoryPlaintextDecodeStatus::kReserved);

  memcpy(bad, bytes, sizeof(bad));
  memset(&bad[4], 0, 8U);
  assert(deserializeBackendDurableReceiptPlaintext(
             bad, sizeof(bad), &decoded) ==
         HistoryPlaintextDecodeStatus::kIdentity);

  memcpy(bad, bytes, sizeof(bad));
  memcpy(&bad[12], &bad[4], 8U);
  assert(deserializeBackendDurableReceiptPlaintext(
             bad, sizeof(bad), &decoded) ==
         HistoryPlaintextDecodeStatus::kIdentityOrder);

  memcpy(bad, bytes, sizeof(bad));
  bad[2] = 1U;
  assert(deserializeBackendDurableReceiptPlaintext(
             bad, sizeof(bad), &decoded) ==
         HistoryPlaintextDecodeStatus::kLength);

  uint8_t count_too_large[kHistoryReceiptMaxPlaintextSize]{};
  count_too_large[0] = kHistoryReceiptSchema;
  count_too_large[1] = kHistoryReceiptScopeBackendDurable;
  count_too_large[2] = kHistoryReceiptMaxIdentities + 1U;
  assert(deserializeBackendDurableReceiptPlaintext(
             count_too_large, sizeof(count_too_large), &decoded) ==
         HistoryPlaintextDecodeStatus::kCount);

  uint8_t thirteen[13] = {
      kHistoryReceiptSchema,kHistoryReceiptScopeBackendDurable,1U,0U,
      0,0,0,0,0,0,0,1,0};
  assert(deserializeBackendDurableReceiptPlaintext(
             thirteen, sizeof(thirteen), &decoded) ==
         HistoryPlaintextDecodeStatus::kLength);

  uint8_t non_multiple[21]{};
  memcpy(non_multiple, bytes, sizeof(bytes));
  assert(deserializeBackendDurableReceiptPlaintext(
             non_multiple, sizeof(non_multiple), &decoded) ==
         HistoryPlaintextDecodeStatus::kLength);

  assert(deserializeBackendDurableReceiptPlaintext(
             nullptr, sizeof(bytes), &decoded) ==
         HistoryPlaintextDecodeStatus::kLength);
  assert(deserializeBackendDurableReceiptPlaintext(
             bytes, sizeof(bytes), nullptr) ==
         HistoryPlaintextDecodeStatus::kLength);
}

}  // namespace

int main() {
  static_assert(kHistorySecureHeaderSize == 36U);
  static_assert(kHistoryObservationPlaintextSize == 29U);
  static_assert(kHistoryReceiptMaxPlaintextSize == 52U);
  static_assert(kHistoryObservationPacketSize == 73U);
  static_assert(kHistoryReceiptMinPacketSize == 56U);
  static_assert(kHistorySecureMinPacketSize == 56U);
  static_assert(kHistorySecureMaxPacketSize == 96U);

  contextDirectionRegistry();
  observationPlaintextGolden();
  observationCoordinateBounds();
  receiptPlaintextGoldenAndBounds();
  observationEnvelopeSecurityVectorGolden();
  receiptEnvelopeSecurityVectorGolden();
  minAndMaxReceiptEnvelope();
  malformedEnvelope();
  malformedObservationPlaintext();
  malformedReceiptPlaintext();
  return 0;
}
