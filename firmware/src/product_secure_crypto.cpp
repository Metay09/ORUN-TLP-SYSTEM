#include "product_secure_crypto.h"

#include <string.h>

#include <nrf_cc310/include/crys_aesccm.h>
#include <nrf_cc310/include/crys_aesccm_error.h>
#include <nrf_cc310/include/crys_error.h>
#include <nrf_cc310/include/crys_hkdf.h>

#include "security_format.h"
#include "security_store.h"
#include "security_traffic_bytes.h"

namespace orun_tlp {
namespace {

void secureZero(void* data, size_t size) {
  volatile uint8_t* bytes = static_cast<volatile uint8_t*>(data);
  while (size-- != 0U) *bytes++ = 0U;
}

// AES-128-CCM encrypt through CC310, the same call sequence as the
// physically qualified HistorySecureCrypto (M7P6I). Seal only.
CRYSError_t aesCcmSeal(CRYS_AESCCM_Key_t key, uint8_t* nonce,
                       uint8_t nonce_size, uint8_t* aad, uint32_t aad_size,
                       uint8_t* input, uint32_t input_size, uint8_t* output,
                       uint8_t* tag, uint8_t tag_size) {
  CRYS_AESCCM_UserContext_t context{};
  CRYSError_t result = CC_AESCCM_Init(
      &context, SASI_AES_ENCRYPT, key, CRYS_AES_Key128BitSize, aad_size,
      input_size, nonce, nonce_size, tag_size, CRYS_AESCCM_MODE_CCM);
  if (result == CRYS_OK && aad_size != 0U)
    result = CRYS_AESCCM_BlockAdata(&context, aad, aad_size);
  if (result == CRYS_OK) {
    CRYS_AESCCM_Mac_Res_t mac{};
    uint8_t finish_tag_size = tag_size;
    result = CRYS_AESCCM_Finish(&context, input, input_size, output, mac,
                                &finish_tag_size);
    if (result == CRYS_OK && finish_tag_size != tag_size)
      result = CRYS_AESCCM_ILLEGAL_PARAMETER_SIZE_ERROR;
    if (result == CRYS_OK) memcpy(tag, mac, tag_size);
    secureZero(mac, sizeof(mac));
  }
  secureZero(&context, sizeof(context));
  return result;
}

}  // namespace

ProductSecureCryptoResult ProductSecureCrypto::protectNextPeriodic(
    uint8_t path_flags, const tlp::PeriodicObservationPlaintext& plaintext,
    uint8_t (&frame)[tlp::kPeriodicFrameSize]) {
  uint8_t bytes[tlp::kPeriodicPlaintextSize]{};
  if (tlp::encodePeriodicPlaintext(plaintext, bytes, sizeof(bytes)) !=
      tlp::ProductSecureCodecResult::kOk)
    return ProductSecureCryptoResult::kInvalidArgument;
  const ProductSecureCryptoResult result =
      seal(tlp::kProductAppFamilyPeriodic, path_flags, bytes, sizeof(bytes),
           frame, sizeof(frame));
  secureZero(bytes, sizeof(bytes));
  return result;
}

ProductSecureCryptoResult ProductSecureCrypto::protectNextEvent(
    uint8_t path_flags, const tlp::EventPlaintext& plaintext,
    uint8_t (&frame)[tlp::kEventFrameSize]) {
  uint8_t bytes[tlp::kEventPlaintextSize]{};
  if (tlp::encodeEventPlaintext(plaintext, bytes, sizeof(bytes)) !=
      tlp::ProductSecureCodecResult::kOk)
    return ProductSecureCryptoResult::kInvalidArgument;
  const ProductSecureCryptoResult result =
      seal(tlp::kProductAppFamilyEvent, path_flags, bytes, sizeof(bytes),
           frame, sizeof(frame));
  secureZero(bytes, sizeof(bytes));
  return result;
}

ProductSecureCryptoResult ProductSecureCrypto::seal(
    uint8_t app_family, uint8_t path_flags, uint8_t* plaintext,
    size_t plaintext_size, uint8_t* frame_out, size_t frame_size) {
  if (!security_store_.ready_ ||
      security_store_.state_ != SecurityState::kProvisioned ||
      security_store_.busy())
    return ProductSecureCryptoResult::kUnavailable;
  if (security_store_.credential_.key_epoch == UINT32_MAX ||
      (path_flags & static_cast<uint8_t>(~tlp::kProductPathFlagsAllowedMask)))
    return ProductSecureCryptoResult::kInvalidArgument;

  // Everything caller-owned is valid; only now consume a nonce counter.
  uint64_t counter = 0;
  uint32_t counter_epoch = 0;
  if (!security_store_.reserveNextTxCounter(counter, counter_epoch))
    return ProductSecureCryptoResult::kUnavailable;
  // The store's first reservation is 0, which PRODUCT_SECURE rejects (as
  // HISTORY_SECURE does): burn it exactly once, never weaken the wire rule.
  if (counter == 0U &&
      !security_store_.reserveNextTxCounter(counter, counter_epoch))
    return ProductSecureCryptoResult::kUnavailable;
  if (counter == 0U || counter_epoch == UINT32_MAX ||
      counter_epoch != security_store_.credential_.key_epoch)
    return ProductSecureCryptoResult::kEngineError;

  tlp::ProductSecureFrame frame;
  frame.app_family = app_family;
  frame.path_flags = path_flags;
  frame.security_profile = tlp::kProductSecurityProfileRootAeadV1;
  frame.ciphertext_len = static_cast<uint8_t>(plaintext_size);
  frame.device_id = security_store_.device_identity_.legacyUint64();
  frame.key_epoch = counter_epoch;
  frame.security_counter = counter;
  uint8_t aad[tlp::kProductSecureHeaderSize]{};
  if (tlp::encodeProductSecureAad(frame, aad) !=
      tlp::ProductSecureCodecResult::kOk)
    return ProductSecureCryptoResult::kEngineError;

  uint8_t root[security_format::kKRootSize]{};
  uint8_t salt[security_format::kCredentialIdSize]{};
  uint8_t info[kSecurityTrafficInfoSize]{};
  uint8_t traffic_key[kSecurityTrafficKeySize]{};
  uint8_t nonce[kSecurityTrafficNonceSize]{};
  CRYS_AESCCM_Key_t ccm_key{};
  memcpy(root, security_store_.credential_.k_root, sizeof(root));
  memcpy(salt, security_store_.credential_.credential_id, sizeof(salt));

  ProductSecureCryptoResult result = ProductSecureCryptoResult::kEngineError;
  if (buildSecurityTrafficInfo(kSecurityTrafficDirectionD2a, counter_epoch,
                               info) &&
      buildSecurityTrafficNonce(kSecurityTrafficDirectionD2a, counter_epoch,
                                counter, nonce) &&
      CRYS_HKDF_KeyDerivFunc(CRYS_HKDF_HASH_SHA256_mode, salt, sizeof(salt),
                             root, sizeof(root), info, sizeof(info),
                             traffic_key, sizeof(traffic_key),
                             SASI_FALSE) == CRYS_OK) {
    memcpy(ccm_key, traffic_key, sizeof(traffic_key));
    if (aesCcmSeal(ccm_key, nonce, sizeof(nonce), aad, sizeof(aad), plaintext,
                   static_cast<uint32_t>(plaintext_size), frame.ciphertext,
                   frame.tag, tlp::kProductSecureTagSize) == CRYS_OK &&
        tlp::encodeProductSecureFrame(frame, frame_out, frame_size) ==
            tlp::ProductSecureCodecResult::kOk)
      result = ProductSecureCryptoResult::kOk;
  }

  secureZero(root, sizeof(root));
  secureZero(salt, sizeof(salt));
  secureZero(info, sizeof(info));
  secureZero(traffic_key, sizeof(traffic_key));
  secureZero(nonce, sizeof(nonce));
  secureZero(ccm_key, sizeof(ccm_key));
  secureZero(frame.ciphertext, sizeof(frame.ciphertext));
  return result;
}

}  // namespace orun_tlp
