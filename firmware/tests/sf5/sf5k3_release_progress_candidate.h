#pragma once

// HOST-ONLY release model. Product ObservationStore makes a cached exact
// object logically invisible as soon as its record's release marker commits.
// Full ACK crypto preflight is a separate prerequisite, NOT implemented here.
// After a crash, only still-retained objects can be reauthorized by a NEW
// subset ACK. Do not assume released ciphertext is available for replay.
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
  bool exact_present = false; // ObservationStore.findExactObject view
  bool exact_matches = false; // validated full exact bytes when present
  bool mutation_uncertain = false;
};
enum class Request : uint8_t { kStarted, kBusy, kRejected };
class StorePort {
 public:
  virtual ~StorePort() {}
  virtual bool inspect(const osf::RecordIdentity& id, DurableView& out) = 0;
  virtual Request requestRelease(const osf::RecordIdentity& id) = 0;
  virtual bool takeReleaseResult(bool& success) = 0;
};
enum class Status : uint8_t { kIdle, kWorking, kBusy, kAwaitingFlash,
                              kFinished, kFault };
class ReleaseProgress {
 public:
  explicit ReleaseProgress(StorePort& port) : port_(port) {}
  // HOST FIXTURE ONLY: caller has cryptographically authenticated the ENTIRE
  // ACK against all full exact frames, the enrolled grant and credential epoch.
  // This also checks the actual store has every pending exact object before
  // beginning. No production caller is permitted until the real crypto gate.
  bool beginHostPreflighted(const osf::RecordIdentity* ids, size_t count);
  Status tick();
  Status status() const { return status_; }
  size_t completed() const { return index_; }
  size_t total() const { return count_; }
 private:
  enum class Phase : uint8_t { kInspect, kAwaitRelease };
  StorePort& port_;
  osf::RecordIdentity members_[kMaxMembers]{};
  size_t count_ = 0U;
  size_t index_ = 0U;
  Phase phase_ = Phase::kInspect;
  bool readback_required_ = false;
  Status status_ = Status::kIdle;
};
} // namespace sf5k3_candidate
} // namespace orun_tlp
