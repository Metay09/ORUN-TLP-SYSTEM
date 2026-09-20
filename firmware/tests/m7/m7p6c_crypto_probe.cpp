// M7P6C target-only crypto known-answer probe.
//
// TEST-ONLY image. It is not part of the production rak4630 environment and
// must never be left on a deployed node. The purpose is to prove that the
// exact Adafruit nRF52 framework pinned by ORUN exposes a working CryptoCell
// CC310 path for the two primitives currently favored by the security ADR:
// RFC5869 HKDF-SHA256 and AES-128-CCM with an 8-byte tag.
//
// The vectors are published standards vectors:
// - HKDF: RFC 5869 Appendix A.1.
// - AES-CCM: RFC 3610 Packet Vector #1.
//
// This probe deliberately does NOT define ORUN wire bytes, HKDF labels, nonce
// layout, traffic-key direction, ACK semantics, replay state, or provisioning.
// Passing it proves only the pinned target crypto implementation against these
// known-answer vectors.
#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <Adafruit_nRFCrypto.h>

#include <string.h>

#include <nrf_cc310/include/crys_aesccm.h>
#include <nrf_cc310/include/crys_aesccm_error.h>
#include <nrf_cc310/include/crys_error.h>
#include <nrf_cc310/include/crys_hkdf.h>

namespace {

bool equalBytes(const uint8_t* a, const uint8_t* b, size_t size) {
  uint8_t diff = 0;
  for (size_t i = 0; i < size; ++i) diff |= static_cast<uint8_t>(a[i] ^ b[i]);
  return diff == 0;
}

bool testHkdfSha256() {
  uint8_t ikm[22];
  memset(ikm, 0x0B, sizeof(ikm));
  uint8_t salt[13];
  for (uint8_t i = 0; i < sizeof(salt); ++i) salt[i] = i;
  uint8_t info[10];
  for (uint8_t i = 0; i < sizeof(info); ++i) info[i] = static_cast<uint8_t>(0xF0U + i);

  static const uint8_t kExpectedOkm[42] = {
      0x3C, 0xB2, 0x5F, 0x25, 0xFA, 0xAC, 0xD5, 0x7A,
      0x90, 0x43, 0x4F, 0x64, 0xD0, 0x36, 0x2F, 0x2A,
      0x2D, 0x2D, 0x0A, 0x90, 0xCF, 0x1A, 0x5A, 0x4C,
      0x5D, 0xB0, 0x2D, 0x56, 0xEC, 0xC4, 0xC5, 0xBF,
      0x34, 0x00, 0x72, 0x08, 0xD5, 0xB8, 0x87, 0x18,
      0x58, 0x65,
  };

  uint8_t okm[sizeof(kExpectedOkm)]{};
  const CRYSError_t result = CRYS_HKDF_KeyDerivFunc(
      CRYS_HKDF_HASH_SHA256_mode, salt, sizeof(salt), ikm, sizeof(ikm), info,
      sizeof(info), okm, sizeof(okm), SASI_FALSE);
  return result == CRYS_OK && equalBytes(okm, kExpectedOkm, sizeof(okm));
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

  if (aad_size > 0) {
    result = CRYS_AESCCM_BlockAdata(&context, aad, aad_size);
    if (result != CRYS_OK) return result;
  }

  // CC310 requires the full 16-byte MAC work buffer. For decrypt, seed its
  // first tag_size bytes with the received tag before Finish(), matching the
  // Nordic SDK CC310 backend integration pattern.
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
  // Dedicated MAC-invalid is the intended API result. The exact pinned
  // nrf_cc310_0.9.13-no-interrupts binary has also been physically observed
  // returning CRYS_FATAL_ERROR for a wrong CCM tag. Keep that compatibility
  // exception narrow: arbitrary nonzero errors are never accepted here.
  return result == CRYS_AESCCM_CCM_MAC_INVALID_ERROR ||
         result == CRYS_FATAL_ERROR;
}

bool testAesCcmEncrypt() {
  CRYS_AESCCM_Key_t key{};
  for (uint8_t i = 0; i < 16; ++i) key[i] = static_cast<uint8_t>(0xC0U + i);

  uint8_t nonce[13] = {
      0x00, 0x00, 0x00, 0x03, 0x02, 0x01, 0x00,
      0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5,
  };
  uint8_t aad[8];
  for (uint8_t i = 0; i < sizeof(aad); ++i) aad[i] = i;

  uint8_t plaintext[23];
  for (uint8_t i = 0; i < sizeof(plaintext); ++i)
    plaintext[i] = static_cast<uint8_t>(0x08U + i);

  static const uint8_t kExpectedCiphertext[23] = {
      0x58, 0x8C, 0x97, 0x9A, 0x61, 0xC6, 0x63, 0xD2,
      0xF0, 0x66, 0xD0, 0xC2, 0xC0, 0xF9, 0x89, 0x80,
      0x6D, 0x5F, 0x6B, 0x61, 0xDA, 0xC3, 0x84,
  };
  static const uint8_t kExpectedTag[8] = {
      0x17, 0xE8, 0xD1, 0x2C, 0xFD, 0xF9, 0x26, 0xE0,
  };

  uint8_t ciphertext[sizeof(plaintext)]{};
  uint8_t tag[sizeof(kExpectedTag)]{};
  const CRYSError_t result = runAesCcm(
      SASI_AES_ENCRYPT, key, nonce, sizeof(nonce), aad, sizeof(aad),
      plaintext, sizeof(plaintext), ciphertext, tag, sizeof(tag));

  return result == CRYS_OK &&
         equalBytes(ciphertext, kExpectedCiphertext, sizeof(ciphertext)) &&
         equalBytes(tag, kExpectedTag, sizeof(kExpectedTag));
}

struct CcmDecryptTamperResult {
  CRYSError_t valid_result = 0;
  bool plaintext_matches = false;
  CRYSError_t tamper_result = 0;
  bool tamper_rejected = false;
  CRYSError_t recovery_result = 0;
  bool recovery_plaintext_matches = false;
  bool negative_matrix_pass = false;
  uint16_t negative_matrix_cases = 0;
  bool forged_stress_pass = false;
  uint16_t forged_stress_iterations = 0;
};

CcmDecryptTamperResult testAesCcmDecryptAndTamper() {
  CcmDecryptTamperResult out{};

  CRYS_AESCCM_Key_t key{};
  for (uint8_t i = 0; i < 16; ++i) key[i] = static_cast<uint8_t>(0xC0U + i);

  uint8_t nonce[13] = {
      0x00, 0x00, 0x00, 0x03, 0x02, 0x01, 0x00,
      0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5,
  };
  uint8_t aad[8];
  for (uint8_t i = 0; i < sizeof(aad); ++i) aad[i] = i;

  uint8_t ciphertext[23] = {
      0x58, 0x8C, 0x97, 0x9A, 0x61, 0xC6, 0x63, 0xD2,
      0xF0, 0x66, 0xD0, 0xC2, 0xC0, 0xF9, 0x89, 0x80,
      0x6D, 0x5F, 0x6B, 0x61, 0xDA, 0xC3, 0x84,
  };
  static const uint8_t kExpectedPlaintext[23] = {
      0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
      0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
      0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E,
  };
  static const uint8_t kExpectedTag[8] = {
      0x17, 0xE8, 0xD1, 0x2C, 0xFD, 0xF9, 0x26, 0xE0,
  };

  uint8_t tag[sizeof(kExpectedTag)]{};
  memcpy(tag, kExpectedTag, sizeof(kExpectedTag));
  uint8_t plaintext[sizeof(ciphertext)]{};
  out.valid_result = runAesCcm(
      SASI_AES_DECRYPT, key, nonce, sizeof(nonce), aad, sizeof(aad),
      ciphertext, sizeof(ciphertext), plaintext, tag, sizeof(tag));
  out.plaintext_matches =
      equalBytes(plaintext, kExpectedPlaintext, sizeof(plaintext));

  auto decryptWithTag = [&](const uint8_t* tag_bytes,
                            bool* plaintext_matches) -> CRYSError_t {
    uint8_t local_tag[sizeof(kExpectedTag)]{};
    memcpy(local_tag, tag_bytes, sizeof(local_tag));
    uint8_t local_plaintext[sizeof(ciphertext)]{};
    const CRYSError_t result = runAesCcm(
        SASI_AES_DECRYPT, key, nonce, sizeof(nonce), aad, sizeof(aad),
        ciphertext, sizeof(ciphertext), local_plaintext, local_tag,
        sizeof(local_tag));
    if (plaintext_matches != nullptr) {
      *plaintext_matches =
          result == CRYS_OK &&
          equalBytes(local_plaintext, kExpectedPlaintext,
                     sizeof(local_plaintext));
    }
    return result;
  };

  auto validDecryptPasses = [&]() -> bool {
    bool match = false;
    return decryptWithTag(kExpectedTag, &match) == CRYS_OK && match;
  };

  // First negative result remains individually visible in serial diagnostics.
  // Never inspect or accept plaintext from a failed authenticated decrypt.
  uint8_t bad_tag[sizeof(kExpectedTag)]{};
  memcpy(bad_tag, kExpectedTag, sizeof(kExpectedTag));
  bad_tag[0] ^= 0x01U;
  out.tamper_result = decryptWithTag(bad_tag, nullptr);
  out.tamper_rejected = isExpectedAuthReject(out.tamper_result);

  // A failed authentication attempt must not poison the crypto engine.
  memcpy(tag, kExpectedTag, sizeof(kExpectedTag));
  memset(plaintext, 0, sizeof(plaintext));
  out.recovery_result = runAesCcm(
      SASI_AES_DECRYPT, key, nonce, sizeof(nonce), aad, sizeof(aad),
      ciphertext, sizeof(ciphertext), plaintext, tag, sizeof(tag));
  out.recovery_plaintext_matches =
      out.recovery_result == CRYS_OK &&
      equalBytes(plaintext, kExpectedPlaintext, sizeof(plaintext));

  // Expanded fail-closed matrix requested by independent review. Each
  // well-formed authenticated-input mutation must be rejected, and after
  // restoring the vector a fresh valid decrypt must still pass.
  bool matrix_ok = true;
  uint16_t matrix_cases = 0;

  for (uint8_t i = 0; i < sizeof(kExpectedTag); ++i) {
    memcpy(bad_tag, kExpectedTag, sizeof(kExpectedTag));
    bad_tag[i] ^= 0x01U;
    ++matrix_cases;
    if (!isExpectedAuthReject(decryptWithTag(bad_tag, nullptr)) ||
        !validDecryptPasses()) {
      matrix_ok = false;
      break;
    }
  }

  auto checkMutatedCurrentInputs = [&]() -> bool {
    ++matrix_cases;
    return isExpectedAuthReject(decryptWithTag(kExpectedTag, nullptr));
  };

  if (matrix_ok) {
    ciphertext[0] ^= 0x01U;
    const bool rejected = checkMutatedCurrentInputs();
    ciphertext[0] ^= 0x01U;
    matrix_ok = rejected && validDecryptPasses();
  }
  if (matrix_ok) {
    ciphertext[sizeof(ciphertext) - 1] ^= 0x01U;
    const bool rejected = checkMutatedCurrentInputs();
    ciphertext[sizeof(ciphertext) - 1] ^= 0x01U;
    matrix_ok = rejected && validDecryptPasses();
  }
  if (matrix_ok) {
    aad[0] ^= 0x01U;
    const bool rejected = checkMutatedCurrentInputs();
    aad[0] ^= 0x01U;
    matrix_ok = rejected && validDecryptPasses();
  }
  if (matrix_ok) {
    nonce[sizeof(nonce) - 1] ^= 0x01U;
    const bool rejected = checkMutatedCurrentInputs();
    nonce[sizeof(nonce) - 1] ^= 0x01U;
    matrix_ok = rejected && validDecryptPasses();
  }
  if (matrix_ok) {
    key[0] ^= 0x01U;
    const bool rejected = checkMutatedCurrentInputs();
    key[0] ^= 0x01U;
    matrix_ok = rejected && validDecryptPasses();
  }

  out.negative_matrix_pass = matrix_ok && matrix_cases == 13U;
  out.negative_matrix_cases = matrix_cases;

  // RF attackers can repeat forgeries. Exercise the pinned negative path
  // repeatedly and require a valid authenticated decrypt immediately after
  // every rejection. No failed-call plaintext is consumed.
  constexpr uint16_t kForgedStressIterations = 1000;
  bool stress_ok = out.negative_matrix_pass;
  uint16_t completed = 0;
  memcpy(bad_tag, kExpectedTag, sizeof(kExpectedTag));
  bad_tag[0] ^= 0x01U;
  for (; stress_ok && completed < kForgedStressIterations; ++completed) {
    if (!isExpectedAuthReject(decryptWithTag(bad_tag, nullptr)) ||
        !validDecryptPasses()) {
      stress_ok = false;
      break;
    }
  }
  out.forged_stress_pass =
      stress_ok && completed == kForgedStressIterations;
  out.forged_stress_iterations = completed;

  return out;
}

}  // namespace

namespace {
bool probe_done = false;
bool probe_pass = false;
bool probe_hkdf = false;
bool probe_ccm_encrypt = false;
bool probe_ccm_decrypt_tamper = false;
CRYSError_t probe_ccm_valid_result = 0;
bool probe_ccm_plaintext_matches = false;
CRYSError_t probe_ccm_tamper_result = 0;
bool probe_ccm_tamper_rejected = false;
CRYSError_t probe_ccm_recovery_result = 0;
bool probe_ccm_recovery_plaintext_matches = false;
bool probe_negative_matrix_pass = false;
uint16_t probe_negative_matrix_cases = 0;
bool probe_forged_stress_pass = false;
uint16_t probe_forged_stress_iterations = 0;
uint32_t last_report_ms = 0;

void reportResult() {
  Serial.printf("M7P6C CRYPTO PROBE %s hkdf=%s ccm_encrypt=%s ccm_decrypt_tamper=%s\n",
                probe_pass ? "PASS" : "FAIL",
                probe_hkdf ? "PASS" : "FAIL",
                probe_ccm_encrypt ? "PASS" : "FAIL",
                probe_ccm_decrypt_tamper ? "PASS" : "FAIL");
  Serial.flush();
  last_report_ms = millis();
}
}  // namespace

void setup() {
  Serial.begin(115200);

  // USB CDC can enumerate more slowly than setup() executes after a reset.
  // Wait a bounded interval for the monitor so one-shot diagnostics are not
  // lost before /dev/ttyACM0 reconnects. The loop also reprints the final
  // result periodically, so reconnect timing cannot hide the KAT outcome.
  const uint32_t serial_wait_started_ms = millis();
  while (!Serial && (millis() - serial_wait_started_ms) < 15000U) delay(10);

  Serial.println(F("M7P6C PROBE BOOT"));
  Serial.println(F("M7P6C CRYPTO INIT START"));
  Serial.flush();
  delay(50);

  if (!nRFCrypto.begin()) {
    Serial.println(F("M7P6C CRYPTO INIT FAIL"));
    Serial.flush();
    probe_done = true;
    return;
  }

  Serial.println(F("M7P6C CRYPTO INIT PASS"));
  Serial.flush();

  Serial.println(F("M7P6C HKDF START"));
  Serial.flush();
  probe_hkdf = testHkdfSha256();
  Serial.printf("M7P6C HKDF %s\n", probe_hkdf ? "PASS" : "FAIL");
  Serial.flush();

  Serial.println(F("M7P6C CCM ENCRYPT START"));
  Serial.flush();
  probe_ccm_encrypt = testAesCcmEncrypt();
  Serial.printf("M7P6C CCM ENCRYPT %s\n", probe_ccm_encrypt ? "PASS" : "FAIL");
  Serial.flush();

  Serial.println(F("M7P6C CCM DECRYPT/TAMPER START"));
  Serial.flush();
  const CcmDecryptTamperResult ccm_decrypt = testAesCcmDecryptAndTamper();
  probe_ccm_valid_result = ccm_decrypt.valid_result;
  probe_ccm_plaintext_matches = ccm_decrypt.plaintext_matches;
  probe_ccm_tamper_result = ccm_decrypt.tamper_result;
  probe_ccm_tamper_rejected = ccm_decrypt.tamper_rejected;
  probe_ccm_recovery_result = ccm_decrypt.recovery_result;
  probe_ccm_recovery_plaintext_matches =
      ccm_decrypt.recovery_plaintext_matches;
  probe_negative_matrix_pass = ccm_decrypt.negative_matrix_pass;
  probe_negative_matrix_cases = ccm_decrypt.negative_matrix_cases;
  probe_forged_stress_pass = ccm_decrypt.forged_stress_pass;
  probe_forged_stress_iterations = ccm_decrypt.forged_stress_iterations;
  probe_ccm_decrypt_tamper =
      probe_ccm_valid_result == CRYS_OK && probe_ccm_plaintext_matches &&
      probe_ccm_tamper_rejected && probe_ccm_recovery_result == CRYS_OK &&
      probe_ccm_recovery_plaintext_matches && probe_negative_matrix_pass &&
      probe_forged_stress_pass;
  Serial.printf(
      "M7P6C CCM DECRYPT valid_rc=0x%08lX plaintext=%s "
      "tamper_rc=0x%08lX expected_mac_invalid=0x%08lX "
      "pinned_generic_fatal=0x%08lX tamper_rejected=%s "
      "recovery_rc=0x%08lX recovery_plaintext=%s\n",
      static_cast<unsigned long>(probe_ccm_valid_result),
      probe_ccm_plaintext_matches ? "MATCH" : "MISMATCH",
      static_cast<unsigned long>(probe_ccm_tamper_result),
      static_cast<unsigned long>(CRYS_AESCCM_CCM_MAC_INVALID_ERROR),
      static_cast<unsigned long>(CRYS_FATAL_ERROR),
      probe_ccm_tamper_rejected ? "YES" : "NO",
      static_cast<unsigned long>(probe_ccm_recovery_result),
      probe_ccm_recovery_plaintext_matches ? "MATCH" : "MISMATCH");
  Serial.printf("M7P6C NEGATIVE MATRIX %s cases=%u/13\n",
                probe_negative_matrix_pass ? "PASS" : "FAIL",
                static_cast<unsigned>(probe_negative_matrix_cases));
  Serial.printf("M7P6C FORGED STRESS %s iterations=%u/1000\n",
                probe_forged_stress_pass ? "PASS" : "FAIL",
                static_cast<unsigned>(probe_forged_stress_iterations));
  Serial.printf("M7P6C CCM DECRYPT/TAMPER %s\n",
                probe_ccm_decrypt_tamper ? "PASS" : "FAIL");
  Serial.flush();

  probe_pass = probe_hkdf && probe_ccm_encrypt && probe_ccm_decrypt_tamper;
  probe_done = true;
  reportResult();

  nRFCrypto.end();
}

void loop() {
  if (probe_done && Serial && (millis() - last_report_ms) >= 3000U) {
    reportResult();
  }
  delay(20);
}
