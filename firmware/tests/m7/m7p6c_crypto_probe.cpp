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
  CRYS_AESCCM_Mac_Res_t tag{};
  const CRYSError_t result = CRYS_AESCCM(
      SASI_AES_ENCRYPT, key, CRYS_AES_Key128BitSize, nonce, sizeof(nonce), aad,
      sizeof(aad), plaintext, sizeof(plaintext), ciphertext,
      sizeof(kExpectedTag), tag);

  return result == CRYS_OK &&
         equalBytes(ciphertext, kExpectedCiphertext, sizeof(ciphertext)) &&
         equalBytes(tag, kExpectedTag, sizeof(kExpectedTag));
}

struct CcmDecryptTamperResult {
  CRYSError_t valid_result = 0;
  bool plaintext_matches = false;
  CRYSError_t tamper_result = 0;
  bool tamper_rejected = false;
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

  CRYS_AESCCM_Mac_Res_t tag{};
  memcpy(tag, kExpectedTag, sizeof(kExpectedTag));
  uint8_t plaintext[sizeof(ciphertext)]{};
  out.valid_result = CRYS_AESCCM(
      SASI_AES_DECRYPT, key, CRYS_AES_Key128BitSize, nonce, sizeof(nonce), aad,
      sizeof(aad), ciphertext, sizeof(ciphertext), plaintext,
      sizeof(kExpectedTag), tag);
  out.plaintext_matches =
      equalBytes(plaintext, kExpectedPlaintext, sizeof(plaintext));

  // Authentication failure must be fail-closed. Do not inspect or accept the
  // plaintext output from this call; only the error result is meaningful.
  CRYS_AESCCM_Mac_Res_t bad_tag{};
  memcpy(bad_tag, kExpectedTag, sizeof(kExpectedTag));
  bad_tag[0] ^= 0x01U;
  memset(plaintext, 0, sizeof(plaintext));
  out.tamper_result = CRYS_AESCCM(
      SASI_AES_DECRYPT, key, CRYS_AES_Key128BitSize, nonce, sizeof(nonce), aad,
      sizeof(aad), ciphertext, sizeof(ciphertext), plaintext,
      sizeof(kExpectedTag), bad_tag);
  out.tamper_rejected =
      out.tamper_result == CRYS_AESCCM_CCM_MAC_INVALID_ERROR;
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
  probe_ccm_decrypt_tamper =
      probe_ccm_valid_result == CRYS_OK && probe_ccm_plaintext_matches &&
      probe_ccm_tamper_rejected;
  Serial.printf(
      "M7P6C CCM DECRYPT valid_rc=0x%08lX plaintext=%s "
      "tamper_rc=0x%08lX expected_tamper_rc=0x%08lX tamper_rejected=%s\n",
      static_cast<unsigned long>(probe_ccm_valid_result),
      probe_ccm_plaintext_matches ? "MATCH" : "MISMATCH",
      static_cast<unsigned long>(probe_ccm_tamper_result),
      static_cast<unsigned long>(CRYS_AESCCM_CCM_MAC_INVALID_ERROR),
      probe_ccm_tamper_rejected ? "YES" : "NO");
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
