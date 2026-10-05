#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "tlp_v2_history_secure.h"

using namespace orun_tlp::tlp;

namespace {

void observationPlaintextGolden() {
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

void receiptPlaintextGoldenAndBounds() {
  BackendDurableReceiptPlaintext receipt{};
  receipt.count = 2U;
  receipt.history_record_identities[0] = 0x0102030405060708ULL;
  receipt.history_record_identities[1] = 0x1112131415161718ULL;

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

HistorySecurePacket baseObservationEnvelope() {
  HistorySecurePacket packet{};
  packet.security_context = kHistorySecurityContextDeviceD2a;
  packet.app_family = kHistoryAppFamilyObservation;
  packet.path_flags = kHistoryPathFlagRelayAllowed;
  packet.ciphertext_len = kHistoryObservationPlaintextSize;
  packet.device_id = 0x1122334455667788ULL;
  packet.key_epoch = 0x01020304U;
  packet.security_counter = 0x0102030405060708ULL;
  packet.history_incarnation = 0xA1A2A3A4A5A6A7A8ULL;
  for (uint8_t i = 0; i < packet.ciphertext_len; ++i)
    packet.ciphertext[i] = i;
  for (uint8_t i = 0; i < kHistorySecureTagSize; ++i)
    packet.tag[i] = static_cast<uint8_t>(0xD0U + i);
  return packet;
}

void envelopeGoldenAndRoundTrip() {
  HistorySecurePacket packet = baseObservationEnvelope();
  uint8_t bytes[kHistoryObservationPacketSize]{};
  assert(sizeof(bytes) == 73U);
  assert(serializeHistorySecurePacket(packet, bytes, sizeof(bytes)));

  const uint8_t expected_header[kHistorySecureHeaderSize] = {
      0x02,0x03,0x01,0x01,0x01,0x1D,0x00,0x00,
      0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,
      0x01,0x02,0x03,0x04,
      0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,
      0xA1,0xA2,0xA3,0xA4,0xA5,0xA6,0xA7,0xA8};
  assert(memcmp(bytes, expected_header, sizeof(expected_header)) == 0);
  for (uint8_t i = 0; i < packet.ciphertext_len; ++i)
    assert(bytes[kHistorySecureHeaderSize + i] == i);
  for (uint8_t i = 0; i < kHistorySecureTagSize; ++i)
    assert(bytes[kHistorySecureHeaderSize + packet.ciphertext_len + i] ==
           static_cast<uint8_t>(0xD0U + i));

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

void maxReceiptEnvelope() {
  HistorySecurePacket packet{};
  packet.security_context = kHistorySecurityContextBackendA2d;
  packet.app_family = kHistoryAppFamilyBackendDurableReceipt;
  packet.path_flags = kHistoryPathFlagRelayAllowed;
  packet.ciphertext_len = kHistoryReceiptMaxPlaintextSize;
  packet.device_id = 0x1122334455667788ULL;
  packet.key_epoch = 7U;
  packet.security_counter = 9U;
  packet.history_incarnation = 11U;
  uint8_t bytes[kHistorySecureMaxPacketSize]{};
  assert(sizeof(bytes) == 96U);
  assert(serializeHistorySecurePacket(packet, bytes, sizeof(bytes)));
  HistorySecurePacket decoded{};
  assert(deserializeHistorySecurePacket(bytes, sizeof(bytes), &decoded) ==
         HistorySecureDecodeStatus::kOk);
}

void malformedEnvelope() {
  HistorySecurePacket packet = baseObservationEnvelope();
  uint8_t bytes[kHistoryObservationPacketSize]{};
  assert(serializeHistorySecurePacket(packet, bytes, sizeof(bytes)));

  uint8_t bad[kHistoryObservationPacketSize]{};

  memcpy(bad, bytes, sizeof(bad));
  bad[0] = 1U;
  HistorySecurePacket decoded{};
  assert(deserializeHistorySecurePacket(bad, sizeof(bad), &decoded) ==
         HistorySecureDecodeStatus::kVersion);

  memcpy(bad, bytes, sizeof(bad));
  bad[1] = 0x7FU;
  assert(deserializeHistorySecurePacket(bad, sizeof(bad), &decoded) ==
         HistorySecureDecodeStatus::kType);

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

  assert(deserializeHistorySecurePacket(bytes, sizeof(bytes) - 1U, &decoded) ==
         HistorySecureDecodeStatus::kLength);
}

void malformedPlaintexts() {
  HistoryObservationPlaintext observation{};
  observation.history_record_identity = 1U;
  observation.position_flags = kHistoryPositionFlagValidFix;
  observation.gnss_utc_epoch_seconds = 1U;
  uint8_t observation_bytes[kHistoryObservationPlaintextSize]{};
  assert(!serializeHistoryObservationPlaintext(
      observation, observation_bytes, sizeof(observation_bytes)));

  observation.gnss_utc_epoch_seconds = 0U;
  observation.position_flags = 0U;
  assert(!serializeHistoryObservationPlaintext(
      observation, observation_bytes, sizeof(observation_bytes)));

  BackendDurableReceiptPlaintext receipt{};
  receipt.count = 2U;
  receipt.history_record_identities[0] = 10U;
  receipt.history_record_identities[1] = 9U;
  uint8_t receipt_bytes[20]{};
  assert(!serializeBackendDurableReceiptPlaintext(
      receipt, receipt_bytes, sizeof(receipt_bytes)));

  const uint8_t bad_receipt[12] = {
      0x01,0x01,0x01,0x00,
      0,0,0,0,0,0,0,0};
  assert(deserializeBackendDurableReceiptPlaintext(
             bad_receipt, sizeof(bad_receipt), &receipt) ==
         HistoryPlaintextDecodeStatus::kIdentity);
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
  observationPlaintextGolden();
  receiptPlaintextGoldenAndBounds();
  envelopeGoldenAndRoundTrip();
  maxReceiptEnvelope();
  malformedEnvelope();
  malformedPlaintexts();
  return 0;
}
