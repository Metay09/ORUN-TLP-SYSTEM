#include "sf5k3_release_progress_candidate.h"

namespace orun_tlp {
namespace sf5k3_candidate {
bool ReleaseProgress::beginHostPreflighted(const osf::RecordIdentity* ids,
                                           size_t count) {
  if (status_ != Status::kIdle || !ids || count == 0U || count > kMaxMembers)
    return false;
  for (size_t i = 0U; i < count; ++i) {
    if (!ids[i].valid()) return false;
    for (size_t j = 0U; j < i; ++j)
      if (ids[i].incarnation == ids[j].incarnation &&
          ids[i].sequence == ids[j].sequence) return false;
  }
  // Store-side preflight, in addition to the required EXTERNAL full-batch
  // SHA-256 / HMAC authorization. No member is released on partial failure.
  for (size_t i = 0U; i < count; ++i) {
    DurableView state{};
    if (!port_.inspect(ids[i], state) || state.mutation_uncertain ||
        !state.record_present || state.released || !state.exact_present ||
        !state.exact_matches) return false;
  }
  for (size_t i = 0U; i < count; ++i) members_[i] = ids[i];
  count_ = count;
  index_ = 0U;
  phase_ = Phase::kInspect;
  readback_required_ = false;
  status_ = Status::kWorking;
  return true;
}

Status ReleaseProgress::tick() {
  if (status_ == Status::kIdle || status_ == Status::kFinished ||
      status_ == Status::kFault) return status_;
  if (index_ >= count_) return status_ = Status::kFinished;
  if (phase_ == Phase::kAwaitRelease) {
    bool success = false;
    if (!port_.takeReleaseResult(success))
      return status_ = Status::kAwaitingFlash;
    if (!success) return status_ = Status::kFault;
    readback_required_ = true;
    phase_ = Phase::kInspect;
    return status_ = Status::kWorking;
  }
  const osf::RecordIdentity& id = members_[index_];
  DurableView state{};
  if (!port_.inspect(id, state) || state.mutation_uncertain ||
      !state.record_present || (state.exact_present && !state.exact_matches))
    return status_ = Status::kFault;
  if (readback_required_) {
    // The real ObservationStore invalidates findExactObject on release.
    // Durable readback is mandatory. No separate cache-clear write exists.
    if (!state.released || state.exact_present)
      return status_ = Status::kFault;
    readback_required_ = false;
    ++index_;
    return status_ = index_ == count_ ? Status::kFinished : Status::kWorking;
  }
  // No re-release of a member whose exact object became inaccessible.
  // After a crash, a NEW subset ACK must authenticate only still-retained
  // objects; absent credentials/ACK, those records stay retained.
  if (state.released || !state.exact_present) return status_ = Status::kFault;
  switch (port_.requestRelease(id)) {
    case Request::kStarted:
      phase_ = Phase::kAwaitRelease;
      return status_ = Status::kAwaitingFlash;
    case Request::kBusy:
      return status_ = Status::kBusy;
    case Request::kRejected:
      return status_ = Status::kFault;
  }
  return status_ = Status::kFault;
}
} // namespace sf5k3_candidate
} // namespace orun_tlp
