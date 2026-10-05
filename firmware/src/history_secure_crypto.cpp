#include "history_secure_crypto.h"

#include <string.h>

#include <nrf_cc310/include/crys_aesccm.h>
#include <nrf_cc310/include/crys_aesccm_error.h>
#include <nrf_cc310/include/crys_error.h>
#include <nrf_cc310/include/crys_hkdf.h>

#include "security_store.h"
#include "security_traffic_bytes.h"

namespace orun_tlp {
namespace {

struct AesCcmCallResult {
  CRYSError_t result = CRYS_OK;
  bool finish_called = false;
};

void secureZero(void* data, size_t size) {
  volatile uint8_t* bytes = static_cast<volatile uint8_t*>(data);
  while (size-- != 0U) *bytes++ = 0U;
}

AesCcmCallResult runAesCcm(
    SaSiAesEncryptMode_t mode, CRYS_AESCCM_Key_t key,
    uint8_t* nonce, uint8_t nonce_size,
    uint8_t* aad, uint32_t aad_size,
    uint8_t* input, uint32_t input_size,
    uint8_t* output, uint8_t* tag, uint8_t tag_size) {
  AesCcmCallResult call{};
  CRYS_AESCCM_UserContext_t context{};

  const auto finish = [&]() {
    secureZero(&context, sizeof(context));
    return call;
  };

  call.result = CC_AESCCM_Init(
      &context, mode, key, CRYS_AES_Key128BitSize, aad_size, input_size,
      nonce, nonce_size, tag_size, CRYS_AESCCM_MODE_CCM);
  if (call.result != CRYS_OK) return finish();

  if (aad_size != 0U) {
    call.result = CRYS_AESCCM_BlockAdata(&context, aad, aad_size);
    if (call.result != CRYS_OK) return finish();
  }

  CRYS_AESCCM_Mac_Res_t mac_buffer{};
  if (mode == SASI_AES_DECRYPT) memcpy(mac_buffer, tag, tag_size);

  uint8_t finish_tag_size = tag_size;
  call.finish_called = true;
  call.result = CRYS_AESCCM_Finish(
      &context, input, input_size, output, mac_buffer, &finish_tag_size);
  if (call.result == CRYS_OK && finish_tag_size != tag_size) {
    call.result = CRYS_AESCCM_ILLEGAL_PARAMETER_SIZE_ERROR;
    return finish();
  }

  if (call.result == CRYS_OK && mode == SASI_AES_ENCRYPT)
    memcpy(tag, mac_buffer, tag_size);

  return finish();
}

bool finishAuthRejected(const AesCcmCallResult& call) {
  if (!call.finish_called) return false;

  // M7P6C/M7P6E physically observed the pinned
  // nrf_cc310_0.9.13-no-interrupts binary returning CRYS_FATAL_ERROR on the
  // wrong-tag Finish/decrypt path. Keep that compatibility exception scoped
  // to this exact authenticated-decrypt Finish result only.
  return call.result == CRYS_AESCCM_CCM_MAC_INVALID_ERROR ||
         call.result == CRYS_FATAL_ERROR;
}

bool receiptCiphertextLengthValid(size_t size) {
  return size >= tlp::kHistoryReceiptFixedPlaintextSize + sizeof(uint64_t) &&
         size <= tlp::kHistoryReceiptMaxPlaintextSize &&
         (size - tlp::kHistoryReceiptFixedPlaintextSize) %
                 sizeof(uint64_t) ==
             0U;
}

}  // namespace

HistorySecureCryptoResult HistorySecureCrypto::protectNextObservation(
    uint64_t history_incarnation, uint8_t path_flags,
    const tlp::HistoryObservationPlaintext& observation,
    tlp::HistorySecurePacket& packet) {
  if (!security_store_.ready_ ||
      security_store_.state_ != SecurityState::kProvisioned ||
      security_store_.busy()) {
    return HistorySecureCryptoResult::kUnavailable;
  }

  if (history_incarnation == 0U ||
      security_store_.credential_.key_epoch == UINT32_MAX ||
      (path_flags & static_cast<uint8_t>(
                        ~tlp::kHistoryPathFlagsAllowedMask)) != 0U) {
    return HistorySecureCryptoResult::kInvalidArgument;
  }

  // Validate all caller-owned application semantics before consuming a
  // nonce-safety counter. Once a counter is returned by SecurityStore it is
  // never reused, even if later crypto work fails.
  uint8_t plaintext[tlp::kHistoryObservationPlaintextSize]{};
  if (!tlp::serializeHistoryObservationPlaintext(
          observation, plaintext, sizeof(plaintext))) {
    return HistorySecureCryptoResult::kInvalidArgument;
  }

  uint64_t security_counter = 0;
  uint32_t counter_epoch = 0;
  if (!security_store_.reserveNextTxCounter(
          security_counter, counter_epoch)) {
    secureZero(plaintext, sizeof(plaintext));
    return HistorySecureCryptoResult::kUnavailable;
  }

  // SecurityStore's historical first reservation begins at counter 0, while
  // the frozen M4P4 HISTORY_SECURE envelope deliberately rejects counter 0.
  // Burn that one value exactly once rather than weakening the wire contract.
  if (security_counter == 0U) {
    if (!security_store_.reserveNextTxCounter(
            security_counter, counter_epoch)) {
      secureZero(plaintext, sizeof(plaintext));
      return HistorySecureCryptoResult::kUnavailable;
    }
  }

  if (security_counter == 0U ||
      counter_epoch != security_store_.credential_.key_epoch ||
      counter_epoch == UINT32_MAX) {
    secureZero(plaintext, sizeof(plaintext));
    return HistorySecureCryptoResult::kEngineError;
  }

  tlp::HistorySecurePacket candidate{};
  candidate.security_context = tlp::kHistorySecurityContextDeviceD2a;
  candidate.app_family = tlp::kHistoryAppFamilyObservation;
  candidate.path_flags = path_flags;
  candidate.ciphertext_len = tlp::kHistoryObservationPlaintextSize;
  candidate.device_id = security_store_.device_identity_.legacyUint64();
  candidate.key_epoch = counter_epoch;
  candidate.security_counter = security_counter;
  candidate.history_incarnation = history_incarnation;

  // Build the exact final header through the frozen codec. Ciphertext/tag are
  // zero at this point; only bytes 0..35 are consumed as AAD.
  uint8_t frame[tlp::kHistoryObservationPacketSize]{};
  if (!tlp::serializeHistorySecurePacket(candidate, frame, sizeof(frame))) {
    secureZero(plaintext, sizeof(plaintext));
    return HistorySecureCryptoResult::kInvalidArgument;
  }

  uint8_t root[security_format::kKRootSize]{};
  uint8_t salt[security_format::kCredentialIdSize]{};
  uint8_t info[kSecurityTrafficInfoSize]{};
  uint8_t traffic_key[kSecurityTrafficKeySize]{};
  uint8_t nonce[kSecurityTrafficNonceSize]{};
  CRYS_AESCCM_Key_t ccm_key{};

  memcpy(root, security_store_.credential_.k_root, sizeof(root));
  memcpy(salt, security_store_.credential_.credential_id, sizeof(salt));

  if (!buildSecurityTrafficInfo(
          kSecurityTrafficDirectionD2a, candidate.key_epoch, info) ||
      !buildSecurityTrafficNonce(
          kSecurityTrafficDirectionD2a, candidate.key_epoch,
          candidate.security_counter, nonce)) {
    secureZero(root, sizeof(root));
    secureZero(salt, sizeof(salt));
    secureZero(info, sizeof(info));
    secureZero(plaintext, sizeof(plaintext));
    secureZero(frame, sizeof(frame));
    return HistorySecureCryptoResult::kInvalidArgument;
  }

  const CRYSError_t hkdf_result = CRYS_HKDF_KeyDerivFunc(
      CRYS_HKDF_HASH_SHA256_mode,
      salt, sizeof(salt),
      root, sizeof(root),
      info, sizeof(info),
      traffic_key, sizeof(traffic_key),
      SASI_FALSE);

  secureZero(root, sizeof(root));
  secureZero(salt, sizeof(salt));
  secureZero(info, sizeof(info));

  if (hkdf_result != CRYS_OK) {
    secureZero(traffic_key, sizeof(traffic_key));
    secureZero(nonce, sizeof(nonce));
    secureZero(plaintext, sizeof(plaintext));
    secureZero(frame, sizeof(frame));
    return HistorySecureCryptoResult::kEngineError;
  }

  memcpy(ccm_key, traffic_key, sizeof(traffic_key));
  const AesCcmCallResult call = runAesCcm(
      SASI_AES_ENCRYPT, ccm_key,
      nonce, sizeof(nonce),
      frame, tlp::kHistorySecureHeaderSize,
      plaintext, sizeof(plaintext),
      candidate.ciphertext,
      candidate.tag, tlp::kHistorySecureTagSize);

  secureZero(ccm_key, sizeof(ccm_key));
  secureZero(traffic_key, sizeof(traffic_key));
  secureZero(nonce, sizeof(nonce));
  secureZero(plaintext, sizeof(plaintext));
  secureZero(frame, sizeof(frame));

  if (call.result != CRYS_OK)
    return HistorySecureCryptoResult::kEngineError;

  packet = candidate;
  return HistorySecureCryptoResult::kOk;
}

HistorySecureCryptoResult HistorySecureCrypto::openBackendDurableReceipt(
    const uint8_t* frame, size_t frame_size,
    uint64_t expected_history_incarnation,
    tlp::HistorySecurePacket& packet,
    tlp::BackendDurableReceiptPlaintext& receipt,
    uint8_t (&authenticated_credential_id)
        [security_format::kCredentialIdSize]) {
  if (frame == nullptr || expected_history_incarnation == 0U)
    return HistorySecureCryptoResult::kInvalidArgument;

  if (!security_store_.ready_ ||
      security_store_.state_ != SecurityState::kProvisioned ||
      security_store_.busy()) {
    return HistorySecureCryptoResult::kUnavailable;
  }

  tlp::HistorySecurePacket candidate{};
  if (tlp::deserializeHistorySecurePacket(
          frame, frame_size, &candidate) !=
      tlp::HistorySecureDecodeStatus::kOk) {
    return HistorySecureCryptoResult::kInvalidArgument;
  }

  if (candidate.security_context != tlp::kHistorySecurityContextBackendA2d ||
      candidate.app_family != tlp::kHistoryAppFamilyBackendDurableReceipt ||
      candidate.device_id != security_store_.device_identity_.legacyUint64() ||
      candidate.key_epoch != security_store_.credential_.key_epoch ||
      candidate.history_incarnation != expected_history_incarnation ||
      !receiptCiphertextLengthValid(candidate.ciphertext_len)) {
    return HistorySecureCryptoResult::kInvalidArgument;
  }

  uint8_t root[security_format::kKRootSize]{};
  uint8_t credential_id_snapshot[security_format::kCredentialIdSize]{};
  uint8_t info[kSecurityTrafficInfoSize]{};
  uint8_t traffic_key[kSecurityTrafficKeySize]{};
  uint8_t nonce[kSecurityTrafficNonceSize]{};
  uint8_t aad[tlp::kHistorySecureHeaderSize]{};
  uint8_t cipher[tlp::kHistoryReceiptMaxPlaintextSize]{};
  uint8_t plaintext[tlp::kHistoryReceiptMaxPlaintextSize]{};
  uint8_t tag[tlp::kHistorySecureTagSize]{};
  CRYS_AESCCM_Key_t ccm_key{};

  memcpy(root, security_store_.credential_.k_root, sizeof(root));
  memcpy(credential_id_snapshot,
         security_store_.credential_.credential_id,
         sizeof(credential_id_snapshot));
  memcpy(aad, frame, sizeof(aad));
  memcpy(cipher, candidate.ciphertext, candidate.ciphertext_len);
  memcpy(tag, candidate.tag, sizeof(tag));

  if (!buildSecurityTrafficInfo(
          kSecurityTrafficDirectionA2d, candidate.key_epoch, info) ||
      !buildSecurityTrafficNonce(
          kSecurityTrafficDirectionA2d, candidate.key_epoch,
          candidate.security_counter, nonce)) {
    secureZero(root, sizeof(root));
    secureZero(credential_id_snapshot, sizeof(credential_id_snapshot));
    secureZero(info, sizeof(info));
    secureZero(aad, sizeof(aad));
    secureZero(cipher, sizeof(cipher));
    secureZero(plaintext, sizeof(plaintext));
    secureZero(tag, sizeof(tag));
    return HistorySecureCryptoResult::kInvalidArgument;
  }

  const CRYSError_t hkdf_result = CRYS_HKDF_KeyDerivFunc(
      CRYS_HKDF_HASH_SHA256_mode,
      credential_id_snapshot, sizeof(credential_id_snapshot),
      root, sizeof(root),
      info, sizeof(info),
      traffic_key, sizeof(traffic_key),
      SASI_FALSE);

  secureZero(root, sizeof(root));
  secureZero(info, sizeof(info));

  if (hkdf_result != CRYS_OK) {
    secureZero(credential_id_snapshot, sizeof(credential_id_snapshot));
    secureZero(traffic_key, sizeof(traffic_key));
    secureZero(nonce, sizeof(nonce));
    secureZero(aad, sizeof(aad));
    secureZero(cipher, sizeof(cipher));
    secureZero(plaintext, sizeof(plaintext));
    secureZero(tag, sizeof(tag));
    return HistorySecureCryptoResult::kEngineError;
  }

  memcpy(ccm_key, traffic_key, sizeof(traffic_key));
  const AesCcmCallResult call = runAesCcm(
      SASI_AES_DECRYPT, ccm_key,
      nonce, sizeof(nonce),
      aad, sizeof(aad),
      cipher, candidate.ciphertext_len,
      plaintext,
      tag, sizeof(tag));

  secureZero(ccm_key, sizeof(ccm_key));
  secureZero(traffic_key, sizeof(traffic_key));
  secureZero(nonce, sizeof(nonce));
  secureZero(aad, sizeof(aad));
  secureZero(cipher, sizeof(cipher));
  secureZero(tag, sizeof(tag));

  if (call.result != CRYS_OK) {
    secureZero(credential_id_snapshot, sizeof(credential_id_snapshot));
    secureZero(plaintext, sizeof(plaintext));
    return finishAuthRejected(call)
               ? HistorySecureCryptoResult::kAuthRejected
               : HistorySecureCryptoResult::kEngineError;
  }

  tlp::BackendDurableReceiptPlaintext decoded_receipt{};
  const auto plaintext_status =
      tlp::deserializeBackendDurableReceiptPlaintext(
          plaintext, candidate.ciphertext_len, &decoded_receipt);
  secureZero(plaintext, sizeof(plaintext));

  if (plaintext_status != tlp::HistoryPlaintextDecodeStatus::kOk) {
    secureZero(credential_id_snapshot, sizeof(credential_id_snapshot));
    return HistorySecureCryptoResult::kInvalidArgument;
  }

  packet = candidate;
  receipt = decoded_receipt;
  memcpy(authenticated_credential_id,
         credential_id_snapshot, sizeof(credential_id_snapshot));
  secureZero(credential_id_snapshot, sizeof(credential_id_snapshot));
  return HistorySecureCryptoResult::kOk;
}

HistorySecureCryptoResult HistorySecureCrypto::openBackendDurableReceipt(
    const uint8_t* frame, size_t frame_size,
    uint64_t expected_history_incarnation,
    AuthenticatedBackendDurableReceipt& output) {
  tlp::HistorySecurePacket packet{};
  tlp::BackendDurableReceiptPlaintext receipt{};
  uint8_t authenticated_credential_id[
      security_format::kCredentialIdSize]{};

  const HistorySecureCryptoResult result = openBackendDurableReceipt(
      frame, frame_size, expected_history_incarnation,
      packet, receipt, authenticated_credential_id);

  if (result != HistorySecureCryptoResult::kOk) {
    secureZero(authenticated_credential_id,
               sizeof(authenticated_credential_id));
    return result;
  }

  output.packet_ = packet;
  output.receipt_ = receipt;
  memcpy(output.authenticated_credential_id_,
         authenticated_credential_id,
         sizeof(authenticated_credential_id));
  secureZero(authenticated_credential_id,
             sizeof(authenticated_credential_id));
  return HistorySecureCryptoResult::kOk;
}

}  // namespace orun_tlp
