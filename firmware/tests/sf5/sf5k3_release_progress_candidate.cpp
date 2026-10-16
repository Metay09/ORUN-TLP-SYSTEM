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
  for (size_t i = 0U; i < count; ++i) members_[i] = ids[i];
  count_ = count;
  index_ = 0U;
  phase_ = Phase::kInspect;
  readback_ = Readback::kNone;
  status_ = Status::kWorking;
  return true;
}

Status ReleaseProgress::tick() {
  if (status_ == Status::kIdle || status_ == Status::kFinished ||
      status_ == Status::kFault) return status_;
  if (index_ >= count_) {
    status_ = Status::kFinished;
    return status_;
  }
  if (phase_ == Phase::kAwaitRelease || phase_ == Phase::kAwaitClear) {
    bool success = false;
    const bool ready = phase_ == Phase::kAwaitRelease
                           ? port_.takeReleaseResult(success)
                           : port_.takeClearResult(success);
    if (!ready) return status_ = Status::kAwaitingFlash;
    if (!success) return status_ = Status::kFault;
    readback_ = phase_ == Phase::kAwaitRelease
                    ? Readback::kRequireReleased : Readback::kRequireCleared;
    phase_ = Phase::kInspect; // never trust callback without persistent readback
    return status_ = Status::kWorking;
  }

  const osf::RecordIdentity& id = members_[index_];
  DurableView state{};
  if (!port_.inspect(id, state) || state.mutation_uncertain ||
      !state.record_present || (state.exact_present && !state.exact_matches))
    return status_ = Status::kFault;
  if ((readback_ == Readback::kRequireReleased && !state.released) ||
      (readback_ == Readback::kRequireCleared && state.exact_present))
    return status_ = Status::kFault;
  readback_ = Readback::kNone;
  // A retained record whose cached exact ciphertext is missing cannot be
  // proven to match this ACK; do not turn it into a release.
  if (!state.released && !state.exact_present)
    return status_ = Status::kFault;

  if (!state.released) {
    switch (port_.requestRelease(id)) {
      case Request::kStarted:
        phase_ = Phase::kAwaitRelease;
        return status_ = Status::kAwaitingFlash;
      case Request::kBusy:
        return status_ = Status::kBusy;
      case Request::kRejected:
        return status_ = Status::kFault;
    }
  }
  if (state.exact_present) {
    // Only after independently confirmed durable release may the exact cache
    // slot be cleared, allowing another pending logical record to be sealed.
    switch (port_.requestClearExact(id)) {
      case Request::kStarted:
        phase_ = Phase::kAwaitClear;
        return status_ = Status::kAwaitingFlash;
      case Request::kBusy:
        return status_ = Status::kBusy;
      case Request::kRejected:
        return status_ = Status::kFault;
    }
  }
  ++index_;
  if (index_ == count_) return status_ = Status::kFinished;
  return status_ = Status::kWorking;
}

} // namespace sf5k3_candidate
} // namespace orun_tlp
