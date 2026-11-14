#pragma once

#include <stddef.h>
#include <stdint.h>

#include "observation_store_format.h"

namespace orun_tlp {
class SecurityStore;
class ProductRecordStore;
class Kq05BackendReceiptReleaseOwner;

// KQ05 experimental Backend-DURABLE identity receipt, never Gateway minted.
// Wire v2/type 9; 24B header + 4B per ordered sequence + 16B MAC.
// This protocol is NOT yet activated in RadioManager or physically qualified.
namespace kq05_receipt {
constexpr uint8_t kVersion = 2U;
constexpr uint8_t kType = 9U;
constexpr size_t kHeaderSize = 24U;
constexpr size_t kMaxRecords = 8U;
constexpr size_t kTagSize = 16U;
constexpr size_t kMaxWireSize = kHeaderSize + 4U*kMaxRecords + kTagSize;
constexpr size_t kMinWireSize = kHeaderSize + 4U + kTagSize;
struct Header {
  uint64_t device_id = 0U;
  uint64_t incarnation = 0U;
  uint32_t key_epoch = UINT32_MAX;
  uint8_t count = 0U;
  uint32_t sequences[kMaxRecords]{};
};
bool decode(const uint8_t* wire, size_t length, Header& output);
}  // namespace kq05_receipt

// An authenticated capability with inaccessible sequence IDs until actual
// SecurityStore root HKDF+HMAC verification. Never created from raw parse.
class AuthenticatedKq05BackendReceipt final {
 public:
  AuthenticatedKq05BackendReceipt() = default;
 private:
  friend class Kq05BackendReceiptVerifier;
  friend class Kq05BackendReceiptReleaseOwner;
  kq05_receipt::Header header_{};
  bool authenticated_ = false;
};

class Kq05BackendReceiptVerifier final {
 public:
  enum class Result : uint8_t {
    kAuthenticated, kInvalid, kUnavailable, kScopeMismatch, kBadMac
  };

  explicit Kq05BackendReceiptVerifier(SecurityStore& security)
      : security_(security) {}
  // Neither an authenticated receipt nor verification releases any record.
  Result verify(const uint8_t* wire, size_t length,
                const ProductRecordStore& records,
                AuthenticatedKq05BackendReceipt& output) const;

 private:
  SecurityStore& security_;
};

}  // namespace orun_tlp
