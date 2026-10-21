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
