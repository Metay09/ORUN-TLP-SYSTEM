#pragma once

#include <stdint.h>
#include "product_record_store.h"
#include "product_secure_crypto.h"
#include "radio_manager.h"

namespace orun_tlp {

// SF5K3V: one-record-per-boot opt-in RF pilot. NOT a production retry scheduler.
// The ProductRecordStore alone owns flash; crypto alone owns the TX nonce;
// RadioManager alone owns SX1262. No ACK/release authority exists here.
class TrackerProductTxPilot {
 public:
  enum class Event : uint8_t {
    kNone, kCachePending, kCacheDurable, kSent, kBlocked
  };

  TrackerProductTxPilot(ProductRecordStore& records,
                        ProductSecureCrypto& crypto, RadioManager& radio)
      : records_(records), crypto_(crypto), radio_(radio) {}

  // Invoke after ProductRecordStore::poll() and higher-priority radio traffic.
  // Passing false still drains a completed cache outcome, but does not
  // initiate crypto/flash/RF. A sent object is never retried this boot.
  Event update(bool eligible);

  // SF5L2: after a TX, allow ONE more selection of the oldest retained
  // record. If the previous record was durably released this is the next
  // one; if not, it is the same record and its already cached exact bytes
  // are re-sent unchanged (never re-sealed). The caller owns pacing and
  // duty-cycle policy. Blocked stays blocked. true = re-armed.
  bool rearm();
  bool sent() const { return phase_ == Phase::kDone; }
  bool blocked() const { return phase_ == Phase::kBlocked; }

 private:
  enum class Phase : uint8_t {
    kSelect, kNeedCache, kWaitingCache, kReady, kDone, kBlocked
  };

  bool storageIdle() const;
  bool sameIdentity(const osf::RecordIdentity& other) const;

  ProductRecordStore& records_;
  ProductSecureCrypto& crypto_;
  RadioManager& radio_;
  Phase phase_ = Phase::kSelect;
  osf::RecordIdentity identity_{};
  osc::ExactObject sealed_{};
};

}  // namespace orun_tlp
