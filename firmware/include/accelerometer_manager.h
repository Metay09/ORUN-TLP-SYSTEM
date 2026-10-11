#pragma once

#include <stdint.h>

#include "accelerometer_config.h"
#include "accelerometer_sample.h"

namespace orun_tlp {

// One sample of the continuous stream. The FIFO carries no timestamps:
// captured_at_ms is estimated from the drain time, the newest sample of a
// drain at the drain time and each older one 100 ms earlier.
struct AccelerometerStreamSample {
  AccelerometerSample sample{};
  // True when continuity with the previously taken sample is not proven:
  // stream start, FIFO overrun, a failed or implausible drain.
  bool continuity_break = false;
};

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
    uint32_t stream_drains = 0;
    uint32_t stream_samples = 0;
    uint32_t stream_overruns = 0;  // FIFO overrun or consumer too slow
    uint32_t stream_timing_errors = 0;  // sample count far from 10 Hz
    uint32_t stream_read_errors = 0;
  };

  void begin(uint32_t now);
  Event poll(uint32_t now);

  bool detectionComplete() const { return detection_complete_; }
  bool detected() const { return detected_; }
  bool faulted() const { return faulted_; }

  // Explicit diagnostic/session owner only; never starts from role/presence.
  bool startRuntimeSession();
  bool takeRuntimeSample(AccelerometerSample* sample);
  void stopRuntimeSession();
  bool runtimeSessionActive() const { return runtime_active_; }
  bool runtimeShutdownConfirmed() const {
    return detection_complete_ && detected_ && !faulted_ &&
           !runtime_active_ && !stream_active_ && state_ == State::kDone;
  }

  // Continuous 10 Hz FIFO stream for the activity service. Exclusive with the
  // one-shot runtime session; the sensor stays in 10 Hz normal mode until
  // stopStream() has confirmed power-down. Up to three failed or implausible
  // drains in a row only break continuity; the next one is a sensor fault.
  bool startStream();
  void stopStream();
  bool streamActive() const { return stream_active_; }
  bool takeStreamSample(AccelerometerStreamSample* sample);

  bool takeProbeSample(AccelerometerSample* sample);
  const Diagnostics& diagnostics() const { return diagnostics_; }

 private:
  enum class State : uint8_t {
    kDetecting,
    kDetectionBackoff,
    kConfiguring,
    kHighResolutionSettling,
    kProbeWait,
    kReadingSample,
    kPoweringDown,
    kFaultCleanup,
    kDone,
    kStreamSettling,
    kStreamWaiting,
    kStreamReading,
  };

  void scheduleDetectionRetry(uint32_t now);
  Event pollStream(uint32_t now);
  Event streamDrainFailed(uint32_t now);
  void pushStreamSample(const AccelerometerSample& sample);
  void clearStreamBuffer();
  void startPowerDown(Event event);
  Event enterFaultCleanup(uint32_t now);
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
  bool runtime_active_ = false;
  bool runtime_sample_ready_ = false;
  AccelerometerSample runtime_sample_{};
  bool discard_next_sample_ = false;
  uint8_t detection_attempts_ = 0;
  uint8_t configuration_step_ = 0;
  uint8_t power_down_attempts_ = 0;
  uint32_t next_action_at_ms_ = 0;
  uint32_t probe_started_at_ms_ = 0;
  AccelerometerSample probe_sample_{};
  Diagnostics diagnostics_{};

  bool stream_active_ = false;
  bool stream_break_pending_ = false;
  uint8_t stream_consecutive_errors_ = 0;
  uint8_t drain_total_ = 0;
  uint8_t drain_read_ = 0;
  uint32_t drain_at_ms_ = 0;
  uint32_t last_drain_at_ms_ = 0;
  // Drained samples waiting for the consumer (one FIFO's worth).
  AccelerometerStreamSample
      stream_buffer_[accelerometer_config::kFifoCapacity]{};
  uint8_t stream_head_ = 0;
  uint8_t stream_count_ = 0;
};

}  // namespace orun_tlp
