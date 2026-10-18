#pragma once
#include <stdint.h>

#include "custody_ack_verifier.h"
#include "observation_store_format.h"

namespace orun_tlp {

class ProductRecordStore;
class SecurityStore;

// Single loop-owned custody release coordinator. ONLY consumes the opaque
// HMAC-authenticated capability, never raw ACK bytes, and checks both live
// SecurityStore grant and real retained/cached exact observation again at
// the release admission boundary.
//
// ProductRecordStore (not this class) polls the FlashBackend asynchronously.
// No radio ISR/callback may invoke this object. No encryption keys are exposed.
class CustodyAckReleaseOwner {
 public:
  enum class Submit : uint8_t { kStarted, kBusy, kRejected };
  enum class Service : uint8_t {
    kIdle, kWaitingFlash, kDurablyReleased, kNotDurable
  };
  struct Diagnostics {
    uint32_t releases_started = 0U;
    uint32_t refused = 0U;
    uint32_t already_busy = 0U;
    uint32_t durable = 0U;
    uint32_t failed = 0U;
  };

  CustodyAckReleaseOwner(SecurityStore& security,
                         ProductRecordStore& product)
      : security_(security), product_(product) {}

  Submit submitAuthenticated(const AuthenticatedCustodyAck& proof);
  Service service();
  bool pending() const { return pending_; }
  const Diagnostics& diagnostics() const { return diagnostics_; }

 private:
  SecurityStore& security_;
  ProductRecordStore& product_;
  bool pending_ = false;
  observation_store_format::RecordIdentity pending_identity_{};
  Diagnostics diagnostics_{};
};

} // namespace orun_tlp
