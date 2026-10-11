// SF5B TEST-ONLY RAK4631 KAT for the production ProductSecureCrypto seam
// (wire contract section 21, gate 4). Runs after Bluefruit.begin() in the
// rak4630_sf5b_product_crypto_probe image, like the M7P6I History KAT: the
// CC310/nRFCrypto lifecycle stays owned by Bluefruit/SoftDevice.
//
// RAM-backed SecurityStore, public vector credential; the physical
// SecurityStore partition is never read or written.

#include "sf5b_product_crypto_probe.h"

#include <string.h>

#include "device_identity.h"
#include "flash_backend.h"
#include "product_secure_crypto.h"
#include "security_format.h"
#include "security_store.h"
#include "sf5b_product_vectors.h"
#include "tlp_v2_product_secure.h"

namespace orun_tlp::sf5b_test {
namespace {

namespace v = orun_tlp::sf5b_vectors;
constexpr size_t kPageSize = 4096U;

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
    for (size_t i = 0; i < size; ++i)
      if ((bytes_[offset + i] & source[i]) != source[i])
        return FlashOpResult::kFailed;
    for (size_t i = 0; i < size; ++i) bytes_[offset + i] &= source[i];
    return FlashOpResult::kDone;
  }

  FlashOpResult erasePage(uint32_t page) override {
    if (page >= 2U) return FlashOpResult::kFailed;
    memset(bytes_ + page * kPageSize, 0xFF, kPageSize);
    return FlashOpResult::kDone;
  }

 private:
  bool initialized_ = false;
  uint8_t bytes_[2U * kPageSize]{};
};

RamSecurityFlash probe_flash;
SecurityStore probe_store(probe_flash, probe_flash);
ProductSecureCrypto probe_crypto(probe_store);

bool settle() {
  for (uint32_t guard = 0; guard < 2000U && probe_store.busy(); ++guard)
    probe_store.poll();
  return !probe_store.busy();
}

bool provision() {
  uint8_t credential_id[security_format::kCredentialIdSize]{};
  uint8_t root[security_format::kKRootSize]{};
  memcpy(credential_id, v::kCredentialId, sizeof(credential_id));
  memcpy(root, v::kRoot, sizeof(root));
  if (!probe_store.commitCredential(credential_id, v::kKeyEpoch, root) ||
      !settle())
    return false;
  bool success = false;
  if (!probe_store.takeCommitResult(success) || !success) return false;
  // The commit starts the first TX reservation; let it finish.
  return settle() && probe_store.state() == SecurityState::kProvisioned &&
         probe_store.currentKeyEpoch() == v::kKeyEpoch;
}

template <size_t N>
bool equalsHex(const uint8_t (&frame)[N], const char* hex) {
  uint8_t expected[N];
  return v::fromHex(hex, expected, N) && memcmp(frame, expected, N) == 0;
}

}  // namespace

ProductCryptoKatResult runProductCryptoKat() {
  ProductCryptoKatResult result;
  if (!probe_store.begin(DeviceIdentity::fromLegacyUint64(v::kDeviceId)) ||
      probe_store.state() != SecurityState::kUnprovisioned)
    return result;

  uint8_t periodic[tlp::kPeriodicFrameSize]{};
  result.unprovisioned_refused =
      probe_crypto.protectNextPeriodic(v::kPathFlags, v::periodic(),
                                       periodic) ==
      ProductSecureCryptoResult::kUnavailable;

  result.provision = provision();
  if (!result.provision) return result;

  result.periodic =
      probe_crypto.protectNextPeriodic(v::kPathFlags, v::periodic(),
                                       periodic) ==
          ProductSecureCryptoResult::kOk &&
      equalsHex(periodic, v::kKatPeriodicFrameHex);
  settle();

  uint8_t event[tlp::kEventFrameSize]{};
  result.event = probe_crypto.protectNextEvent(v::kPathFlags, v::event(),
                                               event) ==
                     ProductSecureCryptoResult::kOk &&
                 equalsHex(event, v::kKatEventFrameHex);
  settle();

  // Refusals happen before a counter is reserved: the next record gets 3.
  tlp::PeriodicObservationPlaintext bad = v::periodic();
  bad.record_sequence = 0;
  const bool bad_plaintext =
      probe_crypto.protectNextPeriodic(v::kPathFlags, bad, periodic) ==
      ProductSecureCryptoResult::kInvalidArgument;
  const bool bad_flags =
      probe_crypto.protectNextPeriodic(0x80, v::periodic(), periodic) ==
      ProductSecureCryptoResult::kInvalidArgument;
  tlp::ProductSecureFrame next;
  result.refused_without_counter =
      bad_plaintext && bad_flags &&
      probe_crypto.protectNextPeriodic(v::kPathFlags, v::periodic(),
                                       periodic) ==
          ProductSecureCryptoResult::kOk &&
      tlp::decodeProductSecureFrame(periodic, sizeof(periodic), &next) ==
          tlp::ProductSecureCodecResult::kOk &&
      next.security_counter == 3U;
  settle();
  return result;
}

}  // namespace orun_tlp::sf5b_test
