#pragma once

#include <stddef.h>
#include <stdint.h>

#include "kq05_backend_receipt.h"

namespace orun_tlp {
class SecurityStore;
class ProductRecordStore;

// No public per-ID deletion. Accept only a full Backend-durable HMAC receipt,
// reverify before EVERY flash mutation, and use the existing ProductRecordStore
// release-marker/readback owner. A lost receipt or reboot cannot produce an
// acknowledgement for a different incarnation or an absent record.
class Kq05BackendReceiptReleaseOwner final {
 public:
  enum class Submit : uint8_t {
    kStarted, kBusy, kAuthenticationRejected, kStorageUnavailable
  };
  enum class Service : uint8_t {
    kIdle, kWaitingStorage, kWaitingFlash, kApplied, kDuplicateOnly, kFailed
  };
  Kq05BackendReceiptReleaseOwner(SecurityStore& security,
                                 ProductRecordStore& records)
      : records_(records), verifier_(security) {}
  Submit submit(const uint8_t* frame, size_t length);
  Service service();
  bool pending() const { return active_; }
  uint8_t releasedCount() const { return released_; }

 private:
  void clear();
  ProductRecordStore& records_;
  Kq05BackendReceiptVerifier verifier_;
  uint8_t wire_[kq05_receipt::kMaxWireSize]{};
  size_t wire_size_ = 0U;
  uint8_t index_ = 0U;
  uint8_t released_ = 0U;
  bool active_ = false;
  bool waiting_flash_ = false;
};

}  // namespace orun_tlp
