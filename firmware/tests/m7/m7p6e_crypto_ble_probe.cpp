// M7P6E TEST-ONLY CryptoCell + Bluefruit coexistence probe helpers.
//
// Built only by env:rak4630_m7p6e_crypto_ble_probe. Production firmware does
// not link this translation unit.
//
// The probe deliberately uses fixed public KAT material from
// m7p6e_candidate_contract.h; it never reads SecurityStore or a real K_root.
// It also deliberately does NOT call nRFCrypto.begin()/end(): Bluefruit owns
// the shared production lifecycle being exercised by this milestone.

#include "m7p6e_crypto_ble_probe.h"
#include "m7p6e_candidate_contract.h"

#include <string.h>

#include <nrf_cc310/include/crys_aesccm.h>
#include <nrf_cc310/include/crys_aesccm_error.h>
#include <nrf_cc310/include/crys_error.h>
#include <nrf_cc310/include/crys_hkdf.h>

namespace orun_tlp {
namespace m7p6e_test {
namespace {

bool equalBytes(const uint8_t* a, const uint8_t* b, size_t size) {
  uint8_t diff = 0;
  for (size_t i = 0; i < size; ++i) {
    diff |= static_cast<uint8_t>(a[i] ^ b[i]);
  }
  return diff == 0;
}

CRYSError_t runAesCcm(SaSiAesEncryptMode_t mode,
                      CRYS_AESCCM_Key_t key,
                      uint8_t* nonce, uint8_t nonce_size,
                      uint8_t* aad, uint32_t aad_size,
                      uint8_t* input, uint32_t input_size,
                      uint8_t* output,
                      uint8_t* tag, uint8_t tag_size) {
  CRYS_AESCCM_UserContext_t context{};
  CRYSError_t result = CC_AESCCM_Init(
      &context, mode, key, CRYS_AES_Key128BitSize, aad_size, input_size,
      nonce, nonce_size, tag_size, CRYS_AESCCM_MODE_CCM);
  if (result != CRYS_OK) return result;

  if (aad_size > 0U) {
    result = CRYS_AESCCM_BlockAdata(&context, aad, aad_size);
    if (result != CRYS_OK) return result;
  }

  CRYS_AESCCM_Mac_Res_t mac_buffer{};
  if (mode == SASI_AES_DECRYPT) memcpy(mac_buffer, tag, tag_size);

  uint8_t finish_tag_size = tag_size;
  result = CRYS_AESCCM_Finish(&context, input, input_size, output,
                              mac_buffer, &finish_tag_size);
  if (result == CRYS_OK && finish_tag_size != tag_size) {
    return CRYS_AESCCM_ILLEGAL_PARAMETER_SIZE_ERROR;
  }
  if (result == CRYS_OK && mode == SASI_AES_ENCRYPT) {
    memcpy(tag, mac_buffer, tag_size);
  }
  return result;
}

bool isExpectedAuthReject(CRYSError_t result) {
  // M7P6C physically observed the exact pinned
  // nrf_cc310_0.9.13-no-interrupts binary returning CRYS_FATAL_ERROR for the
  // wrong-tag Finish/decrypt path. Keep this compatibility exception scoped
  // to this KAT path only. Any other operation treats FATAL as engine failure.
  return result == CRYS_AESCCM_CCM_MAC_INVALID_ERROR ||
         result == CRYS_FATAL_ERROR;
}

bool deriveTrafficKey(uint8_t direction, uint8_t out[kTrafficKeySize]) {
  uint8_t root[kRootSize]{};
  uint8_t salt[kCredentialIdSize]{};
  uint8_t info[kInfoSize]{};
  memcpy(root, kRoot, sizeof(root));
  memcpy(salt, kCredentialId, sizeof(salt));
  buildInfo(direction, info);

  return CRYS_HKDF_KeyDerivFunc(
             CRYS_HKDF_HASH_SHA256_mode,
             salt, sizeof(salt),
             root, sizeof(root),
             info, sizeof(info),
             out, kTrafficKeySize,
             SASI_FALSE) == CRYS_OK;
}

}  // namespace

KatResult runCandidateKat() {
  KatResult out{};

  uint8_t d2a_key[kTrafficKeySize]{};
  uint8_t a2d_key[kTrafficKeySize]{};
  out.hkdf_d2a =
      deriveTrafficKey(kDirectionD2A, d2a_key) &&
      equalBytes(d2a_key, kExpectedD2AKey, sizeof(d2a_key));
  out.hkdf_a2d =
      deriveTrafficKey(kDirectionA2D, a2d_key) &&
      equalBytes(a2d_key, kExpectedA2DKey, sizeof(a2d_key));
  out.direction_separated =
      !equalBytes(d2a_key, a2d_key, sizeof(d2a_key));

  uint8_t nonce[kNonceSize]{};
  buildNonce(kDirectionD2A, kTxCounter, nonce);
  static const uint8_t kExpectedNonce[kNonceSize] = {
      0x01, 0x02, 0x03, 0x04, 0x01,
      0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
  };
  out.nonce_layout =
      equalBytes(nonce, kExpectedNonce, sizeof(kExpectedNonce));

  CRYS_AESCCM_Key_t key{};
  memcpy(key, d2a_key, kTrafficKeySize);

  uint8_t aad[kAadSize]{};
  uint8_t plaintext[kPlaintextSize]{};
  memcpy(aad, kProbeAad, sizeof(aad));
  memcpy(plaintext, kPlaintext, sizeof(plaintext));

  uint8_t ciphertext[kPlaintextSize]{};
  uint8_t tag[kTagSize]{};
  CRYSError_t result = runAesCcm(
      SASI_AES_ENCRYPT, key, nonce, sizeof(nonce),
      aad, sizeof(aad),
      plaintext, sizeof(plaintext),
      ciphertext, tag, sizeof(tag));
  out.ccm_encrypt =
      result == CRYS_OK &&
      equalBytes(ciphertext, kExpectedCiphertext, sizeof(ciphertext)) &&
      equalBytes(tag, kExpectedTag, sizeof(tag));

  uint8_t decrypt_input[kPlaintextSize]{};
  uint8_t decrypt_output[kPlaintextSize]{};
  uint8_t decrypt_tag[kTagSize]{};
  memcpy(decrypt_input, kExpectedCiphertext, sizeof(decrypt_input));
  memcpy(decrypt_tag, kExpectedTag, sizeof(decrypt_tag));
  result = runAesCcm(
      SASI_AES_DECRYPT, key, nonce, sizeof(nonce),
      aad, sizeof(aad),
      decrypt_input, sizeof(decrypt_input),
      decrypt_output, decrypt_tag, sizeof(decrypt_tag));
  out.ccm_decrypt =
      result == CRYS_OK &&
      equalBytes(decrypt_output, kPlaintext, sizeof(decrypt_output));

  uint8_t bad_tag[kTagSize]{};
  uint8_t rejected_output[kPlaintextSize]{};
  memcpy(bad_tag, kExpectedTag, sizeof(bad_tag));
  bad_tag[0] ^= 0x01U;
  memcpy(decrypt_input, kExpectedCiphertext, sizeof(decrypt_input));
  out.tamper_result = static_cast<uint32_t>(runAesCcm(
      SASI_AES_DECRYPT, key, nonce, sizeof(nonce),
      aad, sizeof(aad),
      decrypt_input, sizeof(decrypt_input),
      rejected_output, bad_tag, sizeof(bad_tag)));
  out.tamper_rejected =
      isExpectedAuthReject(static_cast<CRYSError_t>(out.tamper_result));
  memset(rejected_output, 0, sizeof(rejected_output));

  memset(decrypt_output, 0, sizeof(decrypt_output));
  memcpy(decrypt_input, kExpectedCiphertext, sizeof(decrypt_input));
  memcpy(decrypt_tag, kExpectedTag, sizeof(decrypt_tag));
  result = runAesCcm(
      SASI_AES_DECRYPT, key, nonce, sizeof(nonce),
      aad, sizeof(aad),
      decrypt_input, sizeof(decrypt_input),
      decrypt_output, decrypt_tag, sizeof(decrypt_tag));
  out.recovery =
      result == CRYS_OK &&
      equalBytes(decrypt_output, kPlaintext, sizeof(decrypt_output));

  out.pass =
      out.hkdf_d2a && out.hkdf_a2d && out.direction_separated &&
      out.nonce_layout && out.ccm_encrypt && out.ccm_decrypt &&
      out.tamper_rejected && out.recovery;
  return out;
}

}  // namespace m7p6e_test
}  // namespace orun_tlp
