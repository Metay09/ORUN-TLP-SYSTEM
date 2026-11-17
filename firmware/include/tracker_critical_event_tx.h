#pragma once

#include <stdint.h>

#include "product_record_store.h"
#include "product_secure_crypto.h"
#include "radio_manager.h"

namespace orun_tlp {

// KQ05 opt-in: CRITICAL EVENT only, via EXISTING durable Gateway custody ACK.
// PERIODIC LIVE/HISTORY remain on TrackerAcklessLiveTx without per-record ACK.
// This owner selects the oldest unreleased EVENT, seals it ONCE, commits exact
// ciphertext to OBS1 BEFORE RF, and retries the identical bytes after timeout.
// A radio TX, Gateway receipt or ACK RX is never itself a Flash release.
// Existing CustodyAckReleaseOwner alone verifies grant/HMAC and commits release.
class TrackerCriticalEventTx final {
 public:
  enum class Result : uint8_t {
    kNone, kCachePending, kCacheDurable, kSent, kDeferred,
    kReadError, kBlocked
  };

  static constexpr uint32_t kRetryMs = 60000U;
  static constexpr uint32_t kDeferredMs = 30000U;
  static constexpr uint8_t kScanBudget = 8U;

  TrackerCriticalEventTx(ProductRecordStore& records,
                         ProductSecureCrypto& crypto, RadioManager& radio)
      : records_(records), crypto_(crypto), radio_(radio) {}

  // Call AFTER ProductRecordStore::poll() / takeOutcome(), and BEFORE routine
  // LIVE TX. The caller still owns real RF duty and grant/role eligibility.
  Result update(uint32_t now_ms, bool eligible);

  bool blocked() const { return phase_ == Phase::kBlocked; }
  bool waitingAck() const { return phase_ == Phase::kWaitingAck; }
  const osf::RecordIdentity& selectedIdentity() const { return selected_; }

 private:
  enum class Phase : uint8_t {
    kSelect, kNeedCache, kWaitingCache, kReady, kWaitingAck, kBlocked
  };
  bool idleStorage() const;
  bool sameSelected(const osf::RecordIdentity& id) const {
    return selected_.incarnation == id.incarnation &&
           selected_.sequence == id.sequence;
  }
  void resetSelection() {
    selected_ = {};
    sealed_ = {};
    phase_ = Phase::kSelect;
    deadline_armed_ = false;
  }
  Result block() {
    sealed_ = {};
    phase_ = Phase::kBlocked;
    return Result::kBlocked;
  }

  ProductRecordStore& records_;
  ProductSecureCrypto& crypto_;
  RadioManager& radio_;
  Phase phase_ = Phase::kSelect;
  osf::RecordIdentity selected_{};
  osc::ExactObject sealed_{};
  uint64_t scan_incarnation_ = 0U;
  uint32_t scan_cursor_ = 0U;
  uint32_t idle_next_ = 0U;
  uint32_t next_attempt_ms_ = 0U;
  bool deadline_armed_ = false;  // '0' is a valid wrapping millisecond deadline
};
}  // namespace orun_tlp
