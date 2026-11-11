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

// 4c-1b (Bluetooth write key and tags): the same two primitives for other
// bounded inputs. The exact-length custody calls above stay as they are.
constexpr size_t kMaxBoundedInfo = 64;
// 6d: a whole LoRa command frame (255) behind its 16-byte domain.
constexpr size_t kMaxBoundedData = 272;
bool hkdfSha256Bounded(const uint8_t (&salt)[16], const uint8_t (&root)[32],
                       const uint8_t* info, size_t info_len,
                       uint8_t (&key)[32]);
bool hmacSha256Bounded(const uint8_t (&key)[32], const uint8_t* data,
                       size_t length, uint8_t (&mac)[32]);
// 4c-2: SHA-256 of a staged area set (at most 1 + 8 + 64 * 8 bytes).
// Loop-owned only (the CryptoCell port uses one static scratch buffer).
constexpr size_t kMaxBoundedHash = 600;
bool sha256Bounded(const uint8_t* data, size_t length, uint8_t (&digest)[32]);
} // namespace custody_crypto
} // namespace orun_tlp
