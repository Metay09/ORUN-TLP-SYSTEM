#pragma once

// SF5K3A: HOST-ONLY crash/retry progress model, not an ACK admission path.
// Only a future independently verified full-batch crypto preflight may submit
// a plan. This module has NO cryptography, credential grant, runtime adapter,
// radio, or authority to release production records.
#include <stddef.h>
#include <stdint.h>
#include "observation_store_format.h"

namespace orun_tlp {
namespace sf5k3_candidate {

namespace osf = observation_store_format;
constexpr size_t kMaxMembers = 4U;

struct DurableView {
  bool record_present = false;
  bool released = false;
  bool exact_present = false;
  bool exact_matches = false; // validated full cached bytes, not identity only
  bool mutation_uncertain = false;
};

enum class Request : uint8_t { kStarted, kBusy, kRejected };

class StorePort {
 public:
  virtual ~StorePort() {}
  // Read a freshly recovered/persisted state, not RAM-intent or TX_DONE.
  // Return false on any read fault, corruption, contradictory record or flash
  // outcome; no API here bypasses ObservationStore's single mutation owner.
  virtual bool inspect(const osf::RecordIdentity& id, DurableView& out) = 0;
  virtual Request requestRelease(const osf::RecordIdentity& id) = 0;
  virtual bool takeReleaseResult(bool& success) = 0;
  virtual Request requestClearExact(const osf::RecordIdentity& id) = 0;
  virtual bool takeClearResult(bool& success) = 0;
};

enum class Status : uint8_t { kIdle, kWorking, kBusy, kAwaitingFlash, kFinished, kFault };

class ReleaseProgress {
 public:
  explicit ReleaseProgress(StorePort& port) : port_(port) {}
  // HOST FIXTURE ONLY: call after authenticating the ENTIRE ACK with matching
  // exact stored bytes, scoped gateway grant/epoch and full digest/HMAC.
  // The stored plan is volatile; after power loss, preflight the ACK again.
  bool beginHostPreflighted(const osf::RecordIdentity* ids, size_t count);
  Status tick();
  Status status() const { return status_; }
  size_t completed() const { return index_; }
  size_t total() const { return count_; }

 private:
  enum class Phase : uint8_t { kInspect, kAwaitRelease, kAwaitClear };
  StorePort& port_;
  osf::RecordIdentity members_[kMaxMembers]{};
  size_t count_ = 0U;
  size_t index_ = 0U;
  Phase phase_ = Phase::kInspect;
  enum class Readback : uint8_t { kNone, kRequireReleased, kRequireCleared };
  Readback readback_ = Readback::kNone;
  Status status_ = Status::kIdle;
};

} // namespace sf5k3_candidate
} // namespace orun_tlp
