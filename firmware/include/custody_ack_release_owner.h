#pragma once
#include <stddef.h>
#include <stdint.h>

#include "custody_ack_verifier.h"
#include "observation_store_format.h"

namespace orun_tlp {

class SecurityStore;
class ProductRecordStore;

// The SINGLE loop-owned security -> flash release dispatcher.
// No public API takes a caller-provided authenticated token or record ID;
// only an actual 56-byte ACK accepted by CustodyAckVerifier is admissible.
// ProductRecordStore::poll() is externally owned by the composition root,
// not driven inside an RF callback or inside submit()/service().
class CustodyAckReleaseOwner {
 public:
  enum class Submit : uint8_t {
    kStarted,
    kBusy,
    kAuthenticationRejected,
    kStorageUnavailable,
  };
  enum class Service : uint8_t {
    kIdle,
    kWaitingFlash,
    kDurablyReleased,
    kFailed,
  };

  CustodyAckReleaseOwner(SecurityStore& security,
                         ProductRecordStore& product)
      : security_(security), product_(product), verifier_(security) {}

  // kStarted means ONLY a release marker operation was submitted.
  // Caller MUST wait until service() returns kDurablyReleased.
  Submit submit(const uint8_t* ack, size_t length);
  Service service();
  bool pending() const { return pending_; }

 private:
  SecurityStore& security_;
  ProductRecordStore& product_;
  CustodyAckVerifier verifier_;
  bool pending_ = false;
  observation_store_format::RecordIdentity pending_identity_{};
};

}  // namespace orun_tlp
