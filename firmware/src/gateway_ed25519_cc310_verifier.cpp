#include "gateway_ed25519_cc310_verifier.h"

#if defined(ORUN_SF5K3AA_TARGET_CRYPTO_PROBE)
#include <string.h>

#include <nrf_cc310/include/crys_ec_edw_api.h>
#include <nrf_cc310/include/crys_error.h>

namespace orun_tlp {
namespace {
void secureWipe(void* dest, size_t n) {
  volatile uint8_t* p = static_cast<volatile uint8_t*>(dest);
  while (n-- != 0U) *p++ = 0U;
}
bool unsuitableKey(const uint8_t (&key)[32]) {
  uint8_t any = 0U, different = 0U;
  for (size_t i = 0; i < sizeof(key); ++i) {
    any |= key[i];
    different |= static_cast<uint8_t>(key[i] ^ 0xFFU);
  }
  return any == 0U || different == 0U;
}
}  // namespace

GatewayEd25519Cc310Verifier::GatewayEd25519Cc310Verifier(
    const uint8_t (&trusted_issuer_public_key)[32]) {
  memcpy(issuer_public_key_, trusted_issuer_public_key,
         sizeof(issuer_public_key_));
}

bool GatewayEd25519Cc310Verifier::verify(
    const uint8_t* canonical_payload, size_t payload_size,
    const uint8_t (&signature)[gateway_grant_format::kProofBytes]) const {
  namespace fmt = gateway_grant_format;
  static_assert(fmt::kProofBytes == CRYS_ECEDW_SIGNATURE_BYTES,
                "OGK1 detached signature must remain standard Ed25519");
  static_assert(sizeof(issuer_public_key_) == CRYS_ECEDW_MOD_SIZE_IN_BYTES,
                "Issuer key must be a raw Ed25519 public key");
  if (canonical_payload == nullptr ||
      payload_size != fmt::kPayloadBytes ||
      unsuitableKey(issuer_public_key_)) return false;

  // Nordic's pinned CRYS_ECEDW_Verify signature requests a mutable message
  // pointer. Copy the authenticated payload rather than const_cast the
  // caller's persistent OGK1 buffer. The plaintext contains scoped HMAC
  // keys; wipe BOTH scratch allocations regardless of verifier outcome.
  uint8_t message[fmt::kPayloadBytes]{};
  CRYS_ECEDW_TempBuff_t context{};
  memcpy(message, canonical_payload, sizeof(message));
  const CRYSError_t rc = CRYS_ECEDW_Verify(
      signature, fmt::kProofBytes, issuer_public_key_,
      sizeof(issuer_public_key_), message, sizeof(message), &context);
  secureWipe(message, sizeof(message));
  secureWipe(&context, sizeof(context));
  // CRYS_OK is verification only, never Gateway authorization by itself.
  return rc == CRYS_OK;
}

}  // namespace orun_tlp
#endif
