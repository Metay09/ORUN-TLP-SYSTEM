#pragma once

#include "accelerometer_manager.h"
#include "activity_quality.h"

namespace orun_tlp {

// One explicitly requested diagnostic window. No I2C, config, RF or persistence.
class ActivityCapture {
 public:
  enum class State : uint8_t { kIdle, kCapturing, kStopping, kReady, kInvalid,
                               kFault };
  enum class StartResult : uint8_t { kStarted, kPending, kAbsent, kFault, kBusy };

  explicit ActivityCapture(AccelerometerManager& manager) : manager_(manager) {}
  StartResult start();
  void poll();
  State state() const { return state_; }
  uint16_t sampleCount() const { return window_.sampleCount(); }
  // A result is exposed only after confirmed shutdown, including INVALID timing.
  const ActivityWindowFeatures* result() const;
  ActivityWindowAssessment assessment() const;

 private:
  AccelerometerManager& manager_;
  ActivityWindow window_{};
  ActivityWindowFeatures features_{};
  ActivityWindowAssessment assessment_{};
  State state_ = State::kIdle;
};

}  // namespace orun_tlp
