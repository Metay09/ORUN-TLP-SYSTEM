#pragma once
#include <stddef.h>
#include <stdint.h>

#include "kq05_tracker_batch_ack_verifier.h"
#include "product_record_store.h"

namespace orun_tlp {
class SecurityStore;

// KQ05 0x0A loop-task owner for independently durable OBS1 release markers.
// The incoming bytes are verified against actual SecurityStore/OBS1 exactly
// ONCE before any mutation. This owner then pins the verified full digests
// in RAM and re-checks the live durable grant and each remaining ciphertext
// immediately before starting its own Flash marker. No batch-range erase.
//
// The authenticated proof is volatile. After a crash, NEVER infer a
// completed group; a fresh Gateway ACK for the remaining exact records is
// needed before any further release. Replay of the full original group may
// fail closed when already released entries have left the exact cache.
class Kq05TrackerBatchReleaseOwner final {
 public:
  enum class Submit : uint8_t {
    kStarted, kBusy, kAuthenticationRejected, kStorageUnavailable
  };
  enum class Service : uint8_t {
    kIdle, kWaitingStorage, kWaitingFlash, kReleasedOne,
    kApplied, kFailed
  };

  Kq05TrackerBatchReleaseOwner(SecurityStore& security,
                               ProductRecordStore& records)
      : security_(security), records_(records), verifier_(security) {}

  // Accepts a wire type 0x0A only after verifying ALL 1..5 exact
  // previously retained frames. Never called inside SX126x RX/ISR.
  Submit submit(const uint8_t* frame, size_t length);

  // ProductRecordStore::poll() is driven separately by the single
  // composition root. kReleasedOne denotes one observed committed marker;
  // kApplied only after all requested identities were durably released.
  Service service();

  bool pending() const { return active_; }
  uint8_t releasedCount() const { return released_; }

 private:
  void clear();
  bool currentGrant() const;
  bool currentExact(uint8_t index,
                    observation_store_control::ExactObject& value) const;

  SecurityStore& security_;
  ProductRecordStore& records_;
  Kq05TrackerBatchAckVerifier verifier_;
  AuthenticatedKq05TrackerBatchAck proof_{};
  uint8_t index_=0U;
  uint8_t released_=0U;
  bool active_=false;
  bool waiting_flash_=false;
};

} // namespace orun_tlp
