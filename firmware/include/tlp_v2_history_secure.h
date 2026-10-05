#pragma once

#include <stddef.h>
#include <stdint.h>

namespace orun_tlp::tlp {

// M4P4 / SF2 compact root-credential secure History transport.
//
// This is deliberately separate from DELEGATED_SECURE_APP. A normal gateway
// may transport these bytes but cannot mint BACKEND_DURABLE authority.
constexpr uint8_t kHistorySecureProtocolVersion = 0x02U;
constexpr uint8_t kPacketTypeHistorySecure = 0x03U;

constexpr uint8_t kHistorySecurityContextDeviceD2a = 0x01U;
constexpr uint8_t kHistorySecurityContextBackendA2d = 0x02U;

constexpr uint8_t kHistoryAppFamilyObservation = 0x01U;
constexpr uint8_t kHistoryAppFamilyBackendDurableReceipt = 0x02U;

constexpr uint8_t kHistoryPathFlagRelayAllowed = 1U << 0;
constexpr uint8_t kHistoryPathFlagsAllowedMask = kHistoryPathFlagRelayAllowed;

constexpr uint8_t kHistoryObservationSchema = 0x01U;
constexpr uint8_t kHistoryReceiptSchema = 0x01U;
constexpr uint8_t kHistoryReceiptScopeBackendDurable = 0x01U;

constexpr size_t kHistorySecureHeaderSize = 36U;
constexpr size_t kHistorySecureTagSize = 8U;
constexpr size_t kHistoryObservationPlaintextSize = 29U;
constexpr size_t kHistoryReceiptFixedPlaintextSize = 4U;
constexpr size_t kHistoryReceiptMaxIdentities = 6U;
constexpr size_t kHistoryReceiptMaxPlaintextSize =
    kHistoryReceiptFixedPlaintextSize +
    kHistoryReceiptMaxIdentities * sizeof(uint64_t);
constexpr size_t kHistorySecureMaxCiphertextSize =
    kHistoryReceiptMaxPlaintextSize;
constexpr size_t kHistoryObservationPacketSize =
    kHistorySecureHeaderSize + kHistoryObservationPlaintextSize +
    kHistorySecureTagSize;
constexpr size_t kHistoryReceiptMinPacketSize =
    kHistorySecureHeaderSize + kHistoryReceiptFixedPlaintextSize +
    sizeof(uint64_t) + kHistorySecureTagSize;
constexpr size_t kHistorySecureMinPacketSize = kHistoryReceiptMinPacketSize;
constexpr size_t kHistorySecureMaxPacketSize =
    kHistorySecureHeaderSize + kHistorySecureMaxCiphertextSize +
    kHistorySecureTagSize;

constexpr uint8_t kHistoryPositionFlagsAllowedMask = 0x07U;
constexpr uint8_t kHistoryPositionFlagValidFix = 1U << 0;
constexpr uint8_t kHistoryPositionFlagValidUtcTime = 1U << 1;
constexpr uint8_t kHistoryPositionFlag3dFix = 1U << 2;

struct HistorySecurePacket {
  uint8_t security_context = 0;
  uint8_t app_family = 0;
  uint8_t path_flags = 0;
  uint8_t ciphertext_len = 0;

  // For DEVICE_D2A this is the authenticated source device. For BACKEND_A2D
  // this is the authenticated target device. Credential binding is checked by
  // the security owner after AEAD authentication.
  uint64_t device_id = 0;
  uint32_t key_epoch = 0;
  uint64_t security_counter = 0;
  uint64_t history_incarnation = 0;

  uint8_t ciphertext[kHistorySecureMaxCiphertextSize]{};
  uint8_t tag[kHistorySecureTagSize]{};
};

struct HistoryObservationPlaintext {
  uint64_t history_record_identity = 0;
  uint32_t gnss_utc_epoch_seconds = 0;
  int32_t latitude_e7 = 0;
  int32_t longitude_e7 = 0;
  int32_t altitude_mm = 0;
  uint16_t hdop_x100 = 0;
  uint8_t satellites = 0;
  uint8_t position_flags = 0;
};

struct BackendDurableReceiptPlaintext {
  uint8_t count = 0;
  uint64_t history_record_identities[kHistoryReceiptMaxIdentities]{};
};

enum class HistorySecureDecodeStatus : uint8_t {
  kOk,
  kLength,
  kVersion,
  kType,
  kSecurityContext,
  kAppFamily,
  kContextFamily,
  kPathFlags,
  kCiphertextLength,
  kReserved,
  kKeyEpoch,
  kSecurityCounter,
  kHistoryIncarnation,
};

enum class HistoryPlaintextDecodeStatus : uint8_t {
  kOk,
  kLength,
  kSchema,
  kScope,
  kReserved,
  kIdentity,
  kIdentityOrder,
  kPositionFlags,
  kPositionFix,
  kPositionTime,
  kCount,
};

bool historySecureContextFamilyAllowed(uint8_t security_context,
                                       uint8_t app_family);

bool validateHistorySecurePacket(const HistorySecurePacket& packet);

bool serializeHistorySecurePacket(const HistorySecurePacket& packet,
                                  uint8_t* output, size_t output_size);

HistorySecureDecodeStatus deserializeHistorySecurePacket(
    const uint8_t* input, size_t input_size, HistorySecurePacket* packet);

bool serializeHistoryObservationPlaintext(
    const HistoryObservationPlaintext& observation,
    uint8_t* output, size_t output_size);

HistoryPlaintextDecodeStatus deserializeHistoryObservationPlaintext(
    const uint8_t* input, size_t input_size,
    HistoryObservationPlaintext* observation);

size_t backendDurableReceiptPlaintextSize(uint8_t count);

bool serializeBackendDurableReceiptPlaintext(
    const BackendDurableReceiptPlaintext& receipt,
    uint8_t* output, size_t output_size);

HistoryPlaintextDecodeStatus deserializeBackendDurableReceiptPlaintext(
    const uint8_t* input, size_t input_size,
    BackendDurableReceiptPlaintext* receipt);

}  // namespace orun_tlp::tlp
