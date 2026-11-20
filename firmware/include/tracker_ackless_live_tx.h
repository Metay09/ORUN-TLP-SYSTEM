#pragma once

#include <stdint.h>

#include "product_record_store.h"
#include "product_secure_crypto.h"
#include "radio_manager.h"

namespace orun_tlp {

// KQ-05 experimental LIVE-only sender. Store first; do not allocate an exact
// custody object and do not ask for per-record ACK. This does NOT release
// retained records, verify receipts, or accept them at the Gateway.
//
// DO NOT ENABLE on field devices until the versioned Gateway durable-ingress,
// authenticated selective receipt and flash checkpoint paths are integrated.
// The existing SF5K3V pilot remains the default when this is disabled.
class TrackerAcklessLiveTx final {
 public:
  enum class Event : uint8_t {
    kNone, kSent, kDeferred, kBadRecord, kReadError, kCryptoError
  };
  static constexpr uint32_t kRetryDelayMs = 30000U;
  // Flash lookups per scheduler turn; resume across loops for long EVENT runs.
  static constexpr uint8_t kLiveLookupBudget = 8U;

  TrackerAcklessLiveTx(ProductRecordStore& records,
                       ProductSecureCrypto& crypto, RadioManager& radio)
      : records_(records), crypto_(crypto), radio_(radio) {}

  // Cooperative loop only; call after ProductRecordStore::poll(). 'eligible'
  // includes caller's radio/role/security authorization and higher priority
  // work. No retry-storm: failure is throttled, newer observations supersede
  // untransmitted LIVE candidates. Confirmed delivery is a separate owner.
  Event update(uint32_t now_ms, bool eligible);

  const osf::RecordIdentity& lastSent() const { return last_sent_; }
  uint32_t sentCount() const { return sent_count_; }
  uint32_t deferredCount() const { return deferred_count_; }
  // A bounded scan past EVENT tail is in progress; HISTORY must not steal
  // the next RF slot before the latest PERIODIC candidate is determined.
  bool liveSearchPending() const { return live_search_cursor_ != 0U; }

 private:
  static bool sameIdentity(const osf::RecordIdentity& a,
                           const osf::RecordIdentity& b) {
    return a.incarnation == b.incarnation && a.sequence == b.sequence;
  }
  void defer(uint32_t now_ms) {
    next_try_ms_ = now_ms + kRetryDelayMs;
    throttled_ = true;
    if (deferred_count_ != UINT32_MAX) ++deferred_count_;
  }

  ProductRecordStore& records_;
  ProductSecureCrypto& crypto_;
  RadioManager& radio_;
  osf::RecordIdentity last_sent_{};
  uint32_t next_try_ms_ = 0U;
  // Last scanned OBS1 next identity prevents a newer EVENT from masking LIVE.
  uint64_t live_search_incarnation_ = 0U;
  uint32_t live_search_next_seq_ = 0U;
  uint32_t live_search_cursor_ = 0U;
  uint32_t sent_count_ = 0U;
  uint32_t deferred_count_ = 0U;
  bool throttled_ = false;
};

}  // namespace orun_tlp
