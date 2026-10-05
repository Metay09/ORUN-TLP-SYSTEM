// M7P6I TEST-ONLY full-production-graph KAT for the actual
// HistorySecureCrypto production class.
//
// Uses RAM-backed SecurityStore state with public fixed vector material.
// It never reads or writes the physical SecurityStore partition.

#include "m7p6i_history_crypto_probe.h"

#include <string.h>

#include "device_identity.h"
#include "flash_backend.h"
#include "history_secure_crypto.h"
#include "security_format.h"
#include "security_store.h"
#include "tlp_v2_history_secure.h"

namespace orun_tlp::m7p6i_test {
namespace {

constexpr uint64_t kDeviceId = UINT64_C(0x1122334455667788);
constexpr uint32_t kKeyEpoch = 0x01020304U;
constexpr uint64_t kIncarnation = UINT64_C(0xA1A2A3A4A5A6A7A8);
constexpr size_t kPageSize = 4096U;
constexpr size_t kRegionSize = 2U * kPageSize;

const uint8_t kCredentialId[security_format::kCredentialIdSize] = {
    0xA0,0xA1,0xA2,0xA3,0xA4,0xA5,0xA6,0xA7,
    0xA8,0xA9,0xAA,0xAB,0xAC,0xAD,0xAE,0xAF};

const uint8_t kRoot[security_format::kKRootSize] = {
    0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
    0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,
    0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,
    0x18,0x19,0x1A,0x1B,0x1C,0x1D,0x1E,0x1F};

class RamSecurityFlash final : public FlashBackend {
 public:
  bool begin() override {
    if (!initialized_) {
      memset(bytes_, 0xFF, sizeof(bytes_));
      initialized_ = true;
    }
    return true;
  }

  bool read(uint32_t offset, void* data, size_t size) const override {
    if (data == nullptr || offset > sizeof(bytes_) ||
        size > sizeof(bytes_) - offset)
      return false;
    memcpy(data, bytes_ + offset, size);
    return true;
  }

  FlashOpResult program(uint32_t offset, const void* data,
                        size_t size) override {
    if (data == nullptr || offset > sizeof(bytes_) ||
        size > sizeof(bytes_) - offset)
      return FlashOpResult::kFailed;
    const uint8_t* source = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
      if ((bytes_[offset + i] & source[i]) != source[i])
        return FlashOpResult::kFailed;
    }
    for (size_t i = 0; i < size; ++i)
      bytes_[offset + i] &= source[i];
    return FlashOpResult::kDone;
  }

  FlashOpResult erasePage(uint32_t page) override {
    if (page >= 2U) return FlashOpResult::kFailed;
    memset(bytes_ + page * kPageSize, 0xFF, kPageSize);
    return FlashOpResult::kDone;
  }

 private:
  bool initialized_ = false;
  uint8_t bytes_[kRegionSize]{};
};

RamSecurityFlash probe_flash;
SecurityStore probe_store(probe_flash, probe_flash);
HistorySecureCrypto probe_crypto(probe_store);

bool settle(SecurityStore& store) {
  for (uint32_t guard = 0; guard < 2000U && store.busy(); ++guard)
    store.poll();
  return !store.busy();
}

bool provisionPublicCredential(SecurityStore& store) {
  const auto identity = DeviceIdentity::fromLegacyUint64(kDeviceId);
  if (!store.begin(identity) ||
      store.state() != SecurityState::kUnprovisioned)
    return false;

  uint8_t credential_id[security_format::kCredentialIdSize]{};
  uint8_t root[security_format::kKRootSize]{};
  memcpy(credential_id, kCredentialId, sizeof(credential_id));
  memcpy(root, kRoot, sizeof(root));

  if (!store.commitCredential(credential_id, kKeyEpoch, root))
    return false;
  if (!settle(store))
    return false;

  bool commit_success = false;
  if (!store.takeCommitResult(commit_success) || !commit_success)
    return false;

  // Credential commit auto-starts the first TX reservation. Let that complete
  // before testing the production crypto seam so its normal busy guard is
  // exercised exactly as SF3 will see it.
  if (!settle(store))
    return false;

  return store.ready() &&
         store.state() == SecurityState::kProvisioned &&
         store.currentKeyEpoch() == kKeyEpoch;
}

tlp::HistoryObservationPlaintext observationVector() {
  tlp::HistoryObservationPlaintext observation{};
  observation.history_record_identity = UINT64_C(0x0102030405060708);
  observation.gnss_utc_epoch_seconds = 0x11223344U;
  observation.latitude_e7 = 0x01020304;
  observation.longitude_e7 = -1;
  observation.altitude_mm = 0x05060708;
  observation.hdop_x100 = 0x1234U;
  observation.satellites = 8U;
  observation.position_flags =
      tlp::kHistoryPositionFlagValidFix |
      tlp::kHistoryPositionFlagValidUtcTime |
      tlp::kHistoryPositionFlag3dFix;
  return observation;
}

const uint8_t kExpectedObservationFrame[tlp::kHistoryObservationPacketSize] = {
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

const uint8_t kReceiptFrame[64] = {
    0x02,0x03,0x01,0x02,0x01,0x14,0x00,0x00,
    0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,
    0x01,0x02,0x03,0x04,0x88,0x77,0x66,0x55,
    0x44,0x33,0x22,0x11,0xA1,0xA2,0xA3,0xA4,
    0xA5,0xA6,0xA7,0xA8,0x19,0x77,0xAF,0xF6,
    0x9F,0x4A,0x3E,0xC1,0xEF,0x72,0xBF,0xD8,
    0x19,0x05,0x86,0x31,0x14,0x0B,0xF1,0x6A,
    0xDC,0x88,0xD6,0xA5,0xFD,0x0A,0xE9,0x62};

bool runObservation(HistorySecureCrypto& crypto) {
  tlp::HistorySecurePacket packet{};
  if (crypto.protectObservation(
          kIncarnation, UINT64_C(0x1122334455667788),
          tlp::kHistoryPathFlagRelayAllowed, observationVector(), packet) !=
      HistorySecureCryptoResult::kOk)
    return false;

  uint8_t frame[tlp::kHistoryObservationPacketSize]{};
  return tlp::serializeHistorySecurePacket(packet, frame, sizeof(frame)) &&
         memcmp(frame, kExpectedObservationFrame, sizeof(frame)) == 0;
}

bool runReceipt(HistorySecureCrypto& crypto) {
  tlp::HistorySecurePacket packet{};
  tlp::BackendDurableReceiptPlaintext receipt{};
  if (crypto.openBackendDurableReceipt(
          kReceiptFrame, sizeof(kReceiptFrame), kIncarnation,
          packet, receipt) != HistorySecureCryptoResult::kOk)
    return false;

  return receipt.count == 2U &&
         receipt.history_record_identities[0] ==
             UINT64_C(0x0102030405060708) &&
         receipt.history_record_identities[1] ==
             UINT64_C(0x1112131415161718) &&
         packet.security_counter == UINT64_C(0x8877665544332211);
}

bool runTamperAndRecovery(HistorySecureCrypto& crypto, bool& recovery) {
  uint8_t bad[sizeof(kReceiptFrame)]{};
  memcpy(bad, kReceiptFrame, sizeof(bad));
  bad[sizeof(bad) - 1U] ^= 0x01U;

  tlp::HistorySecurePacket sentinel_packet{};
  sentinel_packet.device_id = UINT64_C(0xDEADBEEFDEADBEEF);
  tlp::BackendDurableReceiptPlaintext sentinel_receipt{};
  sentinel_receipt.count = 5U;

  const auto rejected = crypto.openBackendDurableReceipt(
      bad, sizeof(bad), kIncarnation, sentinel_packet, sentinel_receipt);
  const bool untouched =
      sentinel_packet.device_id == UINT64_C(0xDEADBEEFDEADBEEF) &&
      sentinel_receipt.count == 5U;

  tlp::HistorySecurePacket packet{};
  tlp::BackendDurableReceiptPlaintext receipt{};
  recovery = crypto.openBackendDurableReceipt(
                 kReceiptFrame, sizeof(kReceiptFrame), kIncarnation,
                 packet, receipt) == HistorySecureCryptoResult::kOk;

  return rejected == HistorySecureCryptoResult::kAuthRejected &&
         untouched;
}

}  // namespace

HistoryCryptoKatResult runHistoryCryptoKat() {
  HistoryCryptoKatResult result{};

  result.provision = provisionPublicCredential(probe_store);
  if (!result.provision) return result;

  result.observation = runObservation(probe_crypto);
  result.receipt = runReceipt(probe_crypto);
  result.tamper_rejected =
      runTamperAndRecovery(probe_crypto, result.recovery);
  return result;
}

}  // namespace orun_tlp::m7p6i_test
