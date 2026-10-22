// SF5K3AA: exact target adapter under HOST CC310 shim whose verification
// is REAL OpenSSL Ed25519. Standalone RAK test separately compiles actual
// pinned vendor headers. Neither path authorizes production Gateway custody.
#include <openssl/evp.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "gateway_ed25519_cc310_verifier.h"
#include <nrf_cc310/include/crys_ec_edw_api.h>

using orun_tlp::GatewayEd25519Cc310Verifier;
using orun_tlp::gateway_grant_format::kPayloadBytes;

namespace {
unsigned g_vendor_calls = 0U;
unsigned g_success = 0U;

const uint8_t kPublicKey[32] = {
  0x03, 0xA1, 0x07, 0xBF, 0xF3, 0xCE, 0x10, 0xBE,
  0x1D, 0x70, 0xDD, 0x18, 0xE7, 0x4B, 0xC0, 0x99,
  0x67, 0xE4, 0xD6, 0x30, 0x9B, 0xA5, 0x0D, 0x5F,
  0x1D, 0xDC, 0x86, 0x64, 0x12, 0x55, 0x31, 0xB8
};
const uint8_t kProof[64] = {
  0xFA, 0xB2, 0xF1, 0x19, 0x3E, 0xE3, 0x0A, 0xA7,
  0x0A, 0xDB, 0xAE, 0xD4, 0x63, 0x0B, 0x7B, 0x00,
  0x1F, 0x8C, 0x7D, 0xC8, 0x50, 0x47, 0x9F, 0xDF,
  0x7B, 0x88, 0x36, 0xE2, 0x16, 0x6D, 0xBA, 0xED,
  0xDB, 0x37, 0xE9, 0x99, 0xCD, 0xFB, 0xAE, 0xA5,
  0x87, 0xE7, 0x3E, 0x6E, 0x07, 0xF2, 0x4F, 0x2E,
  0x06, 0xC1, 0x3B, 0x67, 0x60, 0x5D, 0xAF, 0xC2,
  0x30, 0x23, 0x8B, 0x28, 0x66, 0xB6, 0x3A, 0x0A
};
}  // namespace

extern "C" CRYSError_t CRYS_ECEDW_Verify(
    const uint8_t* proof, size_t proof_size,
    const uint8_t* key, size_t key_size,
    uint8_t* message, size_t bytes,
    CRYS_ECEDW_TempBuff_t* context) {
  ++g_vendor_calls;
  if (proof == nullptr || proof_size != 64U || key == nullptr ||
      key_size != 32U || message == nullptr ||
      bytes != kPayloadBytes || context == nullptr)
    return 2U;

  EVP_PKEY* public_key =
      EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, key, key_size);
  if (!public_key) return 3U;
  EVP_MD_CTX* digest = EVP_MD_CTX_new();
  const bool valid =
      digest && EVP_DigestVerifyInit(
          digest, nullptr, nullptr, nullptr, public_key) == 1 &&
      EVP_DigestVerify(digest, proof, proof_size, message, bytes) == 1;
  EVP_MD_CTX_free(digest);
  EVP_PKEY_free(public_key);
  if (valid) ++g_success;
  return valid ? CRYS_OK : 4U;
}

int main() {
  uint8_t data[kPayloadBytes]{};
  for (size_t i = 0; i < sizeof(data); ++i)
    data[i] = static_cast<uint8_t>(i * 17U + 3U);
  GatewayEd25519Cc310Verifier verifier(kPublicKey);
  assert(verifier.verify(data, sizeof(data), kProof));
  assert(g_success == 1U);

  uint8_t changed_sig[64]{};
  memcpy(changed_sig, kProof, sizeof(changed_sig));
  changed_sig[0] ^= 0x01U;
  assert(!verifier.verify(data, sizeof(data), changed_sig));
  data[1823] ^= 0x01U;
  assert(!verifier.verify(data, sizeof(data), kProof));
  data[1823] ^= 0x01U;

  uint8_t wrong_public_key[32]{};
  memcpy(wrong_public_key, kPublicKey, sizeof(wrong_public_key));
  wrong_public_key[31] ^= 0x01U;
  GatewayEd25519Cc310Verifier untrusted(wrong_public_key);
  assert(!untrusted.verify(data, sizeof(data), kProof));

  uint8_t zero_key[32]{};
  GatewayEd25519Cc310Verifier rejected_key(zero_key);
  const unsigned before_invalid = g_vendor_calls;
  assert(!rejected_key.verify(data, sizeof(data), kProof));
  assert(!verifier.verify(data, sizeof(data) - 1U, kProof));
  assert(!verifier.verify(nullptr, sizeof(data), kProof));
  assert(g_vendor_calls == before_invalid);

  assert(verifier.verify(data, sizeof(data), kProof));
  assert(g_success == 2U && g_vendor_calls == 5U);
  puts("SF5K3AA CC310 API adapter/Ed25519 positive-tamper tests: PASS (HOST vendor shim)");
  return 0;
}
