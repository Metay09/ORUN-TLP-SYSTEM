#pragma once

#include <stdint.h>

#include "accelerometer_sample.h"

namespace orun_tlp {

class AccelerometerManager {
 public:
  enum class Event : uint8_t {
    kNone,
    kPresent,
    kAbsent,
    kFault,
  };

  struct Diagnostics {
    uint32_t detection_attempts = 0;
    uint32_t detection_retries = 0;
    uint32_t identity_mismatches = 0;
    uint32_t i2c_timeouts = 0;
    uint32_t i2c_recoveries = 0;
    uint32_t i2c_recovery_failures = 0;
    uint32_t configuration_failures = 0;
    uint32_t sample_failures = 0;
    uint32_t power_down_failures = 0;
    uint32_t probe_samples = 0;
  };

  void begin(uint32_t now);
  Event poll(uint32_t now);

  bool detectionComplete() const { return detection_complete_; }
  bool detected() const { return detected_; }
  bool faulted() const { return faulted_; }

  bool takeProbeSample(AccelerometerSample* sample);
  const Diagnostics& diagnostics() const { return diagnostics_; }

 private:
  enum class State : uint8_t {
    kDetecting,
    kDetectionBackoff,
    kConfiguring,
    kProbeWait,
    kReadingSample,
    kPoweringDown,
    kDone,
  };

  void scheduleDetectionRetry(uint32_t now);
  Event finishAbsent();
  Event finishFault();
  Event finishPresent();

  State state_ = State::kDetecting;
  Event pending_finish_event_ = Event::kNone;
  bool detection_complete_ = false;
  bool detected_ = false;
  bool faulted_ = false;
  bool saw_transport_timeout_ = false;
  bool probe_sample_ready_ = false;
  bool discard_next_sample_ = false;
  uint8_t detection_attempts_ = 0;
  uint8_t configuration_step_ = 0;
  uint32_t next_action_at_ms_ = 0;
  uint32_t probe_started_at_ms_ = 0;
  AccelerometerSample probe_sample_{};
  Diagnostics diagnostics_{};
};

}  // namespace orun_tlp
