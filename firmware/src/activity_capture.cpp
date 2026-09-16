#include "activity_capture.h"

namespace orun_tlp {

ActivityCapture::StartResult ActivityCapture::start() {
  if (state_ == State::kCapturing || state_ == State::kStopping)
    return StartResult::kBusy;
  if (!manager_.detectionComplete()) return StartResult::kPending;
  if (manager_.faulted()) return StartResult::kFault;
  if (!manager_.detected()) return StartResult::kAbsent;
  if (!manager_.startRuntimeSession()) return StartResult::kBusy;
  window_.reset();
  features_ = ActivityWindowFeatures{};
  assessment_ = ActivityWindowAssessment{};
  state_ = State::kCapturing;
  return StartResult::kStarted;
}

void ActivityCapture::poll() {
  if (state_ != State::kCapturing && state_ != State::kStopping) return;
  if (manager_.faulted()) {
    features_ = ActivityWindowFeatures{};
    assessment_ = ActivityWindowAssessment{};
    state_ = State::kFault;
    return;
  }
  if (state_ == State::kCapturing) {
    AccelerometerSample sample{};
    if (manager_.takeRuntimeSample(&sample)) {
      window_.addSample(sample);
      if (window_.complete()) {
        features_ = window_.features();
        assessment_ = assessActivityWindow(features_);
        manager_.stopRuntimeSession();
        state_ = State::kStopping;
      }
    }
    return;
  }
  if (manager_.runtimeShutdownConfirmed())
    state_ = assessment_.usable ? State::kReady : State::kInvalid;
}

const ActivityWindowFeatures* ActivityCapture::result() const {
  return state_ == State::kReady || state_ == State::kInvalid ? &features_
                                                          : nullptr;
}

ActivityWindowAssessment ActivityCapture::assessment() const {
  return result() != nullptr ? assessment_ : ActivityWindowAssessment{};
}

}  // namespace orun_tlp
