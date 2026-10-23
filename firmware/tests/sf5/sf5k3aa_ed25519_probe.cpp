// SF5K3AA standalone RAK4630 CryptoCell Ed25519 known-answer probe.
// Test-image only: fixed PUBLIC test key + detached signature of a synthetic
// 1824-byte OGK1 canonical payload. No real enrollment/private key/flash RF.
// The normal product main.cpp is not linked into this target.
#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <Adafruit_nRFCrypto.h>

#include "gateway_ed25519_cc310_verifier.h"

namespace {
constexpr size_t kBytes = orun_tlp::gateway_grant_format::kPayloadBytes;
static_assert(kBytes == 1824U, "OGK1 signed canonical size changed");

// Test-only public key. This is NOT a field issuer trust root.
const uint8_t kPublic[32] = {
  0x03, 0xA1, 0x07, 0xBF, 0xF3, 0xCE, 0x10, 0xBE,
  0x1D, 0x70, 0xDD, 0x18, 0xE7, 0x4B, 0xC0, 0x99,
  0x67, 0xE4, 0xD6, 0x30, 0x9B, 0xA5, 0x0D, 0x5F,
  0x1D, 0xDC, 0x86, 0x64, 0x12, 0x55, 0x31, 0xB8
};
const uint8_t kSignature[64] = {
  0xFA, 0xB2, 0xF1, 0x19, 0x3E, 0xE3, 0x0A, 0xA7,
  0x0A, 0xDB, 0xAE, 0xD4, 0x63, 0x0B, 0x7B, 0x00,
  0x1F, 0x8C, 0x7D, 0xC8, 0x50, 0x47, 0x9F, 0xDF,
  0x7B, 0x88, 0x36, 0xE2, 0x16, 0x6D, 0xBA, 0xED,
  0xDB, 0x37, 0xE9, 0x99, 0xCD, 0xFB, 0xAE, 0xA5,
  0x87, 0xE7, 0x3E, 0x6E, 0x07, 0xF2, 0x4F, 0x2E,
  0x06, 0xC1, 0x3B, 0x67, 0x60, 0x5D, 0xAF, 0xC2,
  0x30, 0x23, 0x8B, 0x28, 0x66, 0xB6, 0x3A, 0x0A
};
uint8_t payload[kBytes]{};
bool done = false, pass = false;
uint32_t last_report = 0U;

void report() {
  Serial.printf("SF5K3AA CC310 ED25519 %s (test public key only)\n",
                pass ? "PASS" : "FAIL");
  Serial.flush();
}
bool knownAnswer() {
  for (size_t i = 0; i < sizeof(payload); ++i)
    payload[i] = static_cast<uint8_t>(i * 17U + 3U);
  orun_tlp::GatewayEd25519Cc310Verifier verifier(kPublic);
  if (!verifier.verify(payload, sizeof(payload), kSignature)) return false;
  uint8_t wrong_signature[64]{};
  memcpy(wrong_signature, kSignature, sizeof(wrong_signature));
  wrong_signature[3] ^= 0x01U;
  const bool wrong_sig_rejected =
      !verifier.verify(payload, sizeof(payload), wrong_signature);
  payload[107] ^= 0x01U;
  const bool wrong_payload_rejected =
      !verifier.verify(payload, sizeof(payload), kSignature);
  payload[107] ^= 0x01U;

  uint8_t wrong_public[32]{};
  memcpy(wrong_public, kPublic, sizeof(wrong_public));
  wrong_public[0] ^= 0x01U;
  orun_tlp::GatewayEd25519Cc310Verifier wrong_key(wrong_public);
  const bool wrong_key_rejected =
      !wrong_key.verify(payload, sizeof(payload), kSignature);
  const bool bad_lengths_rejected =
      !verifier.verify(payload, sizeof(payload) - 1U, kSignature) &&
      !verifier.verify(nullptr, sizeof(payload), kSignature);
  return wrong_sig_rejected && wrong_payload_rejected &&
         wrong_key_rejected && bad_lengths_rejected &&
         verifier.verify(payload, sizeof(payload), kSignature);
}
}  // namespace

void setup() {
  Serial.begin(115200);
  const uint32_t start = millis();
  while (!Serial && (millis() - start) < 15000U) delay(10U);
  Serial.println(F("SF5K3AA probe: no flash read/write, no LoRa, no issuer enrollment"));
  Serial.flush();
  if (!nRFCrypto.begin()) {
    Serial.println(F("SF5K3AA CRYPTO INIT FAIL"));
    done = true;
    return;
  }
  pass = knownAnswer();
  done = true;
  report();
  nRFCrypto.end();
}

void loop() {
  if (done && Serial && millis() - last_report >= 3000U) {
    last_report = millis();
    report();
  }
  delay(20U);
}
