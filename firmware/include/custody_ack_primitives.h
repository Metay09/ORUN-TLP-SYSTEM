#pragma once
#include <stddef.h>
#include <stdint.h>

// Low-level crypto provider calls. The production definitions use CC310.
// The host KAT links a distinct test-only OpenSSL provider to prove exactly
// the same byte contract, without changing production firmware.
namespace orun_tlp {
namespace custody_crypto {
bool sha256(const uint8_t* data, size_t length, uint8_t (&digest)[32]);
bool hkdfSha256(const uint8_t (&salt)[16], const uint8_t (&root)[32],
                const uint8_t* info, size_t info_len, uint8_t (&key)[32]);
bool hmacSha256(const uint8_t (&key)[32], const uint8_t* data,
                size_t length, uint8_t (&mac)[32]);
} // namespace custody_crypto
} // namespace orun_tlp
