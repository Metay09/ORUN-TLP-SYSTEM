#pragma once

#include <stddef.h>
#include <stdint.h>

#include "tlp_v2_product_secure.h"

namespace orun_tlp {

class SecurityStore;

enum class ProductSecureCryptoResult : uint8_t {
  kOk,
  kUnavailable,      // store not ready/provisioned/busy, or no counter
  kInvalidArgument,  // path flags or plaintext refused by the codec
  kEngineError,      // CC310/HKDF/CCM failure or counter/epoch mismatch
};

// SF5B PRODUCT_SECURE profile 0x01 sealing seam
// (ORUN_TLP_V2_PRODUCT_SECURE_WIRE.md section 3.2): the existing M7P6 root
// D2A construction -- HKDF-SHA256(salt = credential_id, IKM = K_root,
// info = "ORUN-TLP-V2-AEAD" || 0x01 || key_epoch) -> AES-128-CCM, 8-byte tag,
// nonce key_epoch || 0x01 || security_counter, AAD = header bytes 0..27.
//
// K_root and the D2A counter stay owned by SecurityStore (friend access, like
// HistorySecureCrypto). Callers cannot supply a counter. The plaintext is
// validated by the codec BEFORE a counter is reserved, so a refused record
// never consumes one; a reserved counter is never reused, even if the crypto
// engine later fails.
//
// Seal only: the tracker never opens its own D2A product records. No RF or
// runtime caller exists before the SF5F cutover; the RAK4631 KAT
// (tests/sf5/sf5b_product_crypto_probe.cpp) is the only caller today.
class ProductSecureCrypto {
 public:
  explicit ProductSecureCrypto(SecurityStore& security_store)
      : security_store_(security_store) {}

  ProductSecureCryptoResult protectNextPeriodic(
      uint8_t path_flags, const tlp::PeriodicObservationPlaintext& plaintext,
      uint8_t (&frame)[tlp::kPeriodicFrameSize]);
  ProductSecureCryptoResult protectNextEvent(
      uint8_t path_flags, const tlp::EventPlaintext& plaintext,
      uint8_t (&frame)[tlp::kEventFrameSize]);

 private:
  ProductSecureCryptoResult seal(uint8_t app_family, uint8_t path_flags,
                                 uint8_t* plaintext, size_t plaintext_size,
                                 uint8_t* frame, size_t frame_size);

  SecurityStore& security_store_;
};

}  // namespace orun_tlp
