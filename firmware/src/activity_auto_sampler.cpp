#include "activity_auto_sampler.h"

#include "activity_classifier.h"
#include "activity_quality.h"
#include "monotonic_time.h"

namespace orun_tlp {
namespace {
void incrementBounded(uint16_t& value) {
  if (value != UINT16_MAX) ++value;
}
}  // namespace

void ActivityAutoSampler::setEnabled(bool enabled, uint32_t now) {
  if (enabled == enabled_) return;
  enabled_ = enabled;
  if (!enabled_) return;  // poll() stops the stream through the manager.
  next_start_at_ms_ = now;
  resetPeriod(now);
  summary_ready_ = false;
  latest_summary_ = ActivityHourSummary{};
}

void ActivityAutoSampler::resetPeriod(uint32_t now) {
  period_started_at_ms_ = now;
  window_.reset();
  usable_windows_ = invalid_windows_ = fault_windows_ = 0;
  unavailable_attempts_ = boundary_discarded_windows_ = 0;
  summed_variance_mg2_ = summed_abs_delta_mg_ = 0;
  summed_coverage_ms_ = 0;
  summed_active_ms_ = 0;
  active_windows_ = transitions_ = 0;
  has_last_window_ = false;
}

void ActivityAutoSampler::consume(const AccelerometerStreamSample& stream) {
  if (stream.continuity_break && window_.sampleCount() != 0) {
    incrementBounded(invalid_windows_);
    window_.reset();
  }
  // A sample taken before this period started belongs to a period that is
  // already closed (it was still in the FIFO at the close). Keep it out.
  if (!monotonic::reached(stream.sample.captured_at_ms, period_started_at_ms_))
    return;

  // FIFO samples are evenly spaced at the output data rate between breaks,
  // so the window timing is the nominal 100 ms spacing; the manager checks
  // the real rate against the MCU clock at every drain.
  if (window_.sampleCount() == 0) window_first_ms_ = stream.sample.captured_at_ms;
  AccelerometerSample stamped = stream.sample;
  stamped.captured_at_ms =
      window_first_ms_ +
      window_.sampleCount() * activity_config::kExpectedSamplePeriodMs;
  window_.addSample(stamped);
  if (!window_.complete()) return;

  ActivityWindowFeatures features;
  window_.takeCompletedFeatures(&features);
  if (assessActivityWindow(features).usable)
    recordWindow(features);
  else
    incrementBounded(invalid_windows_);
}

void ActivityAutoSampler::recordWindow(const ActivityWindowFeatures& features) {
  incrementBounded(usable_windows_);
  summed_coverage_ms_ += activity_auto_config::kWindowCoverageMs;
  summed_variance_mg2_ += features.axis_variance_sum_mg2;
  summed_abs_delta_mg_ += features.mean_abs_delta_mg;
  last_magnitude_mg2_ = features.mean_magnitude_squared_mg2;
  const bool active = activityWindowActive(features.axis_variance_sum_mg2);
  if (active) {
    incrementBounded(active_windows_);
    summed_active_ms_ += activity_auto_config::kWindowCoverageMs;
  }
  if (has_last_window_ && active != last_window_active_)
    incrementBounded(transitions_);
  has_last_window_ = true;
  last_window_active_ = active;
}

bool ActivityAutoSampler::publishIfDue(uint32_t now) {
  if (report_owned_ ||
      !monotonic::elapsed(now, period_started_at_ms_,
                          activity_auto_config::kSummaryPeriodMs)) {
    return false;
  }
  publish(now);
  return true;
}

bool ActivityAutoSampler::closePeriod(uint32_t now) {
  if (!enabled_ || !monotonic::elapsed(now, period_started_at_ms_, 1000U))
    return false;
  publish(now);
  return true;
}

void ActivityAutoSampler::publish(uint32_t now) {
  // The window still filling belongs to this period but is incomplete.
  if (window_.sampleCount() != 0) incrementBounded(boundary_discarded_windows_);
  ActivityHourSummary summary{};
  summary.started_at_ms = period_started_at_ms_;
  summary.finished_at_ms = now;
  const uint32_t elapsed_ms = now - period_started_at_ms_;
  summary.duration_seconds = elapsed_ms / 1000U;
  summary.measured_coverage_ms = static_cast<uint32_t>(
      summed_coverage_ms_ > elapsed_ms ? elapsed_ms : summed_coverage_ms_);
  summary.usable_windows = usable_windows_;
  summary.invalid_windows = invalid_windows_;
  summary.fault_windows = fault_windows_;
  summary.unavailable_attempts = unavailable_attempts_;
  summary.boundary_discarded_windows = boundary_discarded_windows_;
  if (usable_windows_ != 0) {
    summary.mean_axis_variance_sum_mg2 =
        static_cast<uint32_t>(summed_variance_mg2_ / usable_windows_);
    summary.mean_abs_delta_mg =
        static_cast<uint32_t>(summed_abs_delta_mg_ / usable_windows_);
  }
  summary.active_windows = active_windows_;
  summary.activity_transitions = transitions_;
  summary.measured_active_ms = static_cast<uint32_t>(
      summed_active_ms_ > summary.measured_coverage_ms
          ? summary.measured_coverage_ms
          : summed_active_ms_);
  latest_summary_ = summary;
  summary_ready_ = true;
  if (published_summaries_ != UINT32_MAX) ++published_summaries_;
  resetPeriod(now);  // No catch-up burst or fabricated lost periods.
}

bool ActivityAutoSampler::poll(AccelerometerManager& manager, uint32_t now) {
  if (!enabled_) {
    if (stream_owned_) {
      manager.stopStream();
      stream_owned_ = false;
      window_.reset();
    }
    return false;
  }

  if (stream_owned_) {
    if (manager.faulted() || !manager.streamActive()) {
      // A sensor fault ended the stream; its partial window is lost.
      incrementBounded(fault_windows_);
      window_.reset();
      stream_owned_ = false;
      next_start_at_ms_ = now + activity_auto_config::kUnavailableRetryMs;
    } else {
      AccelerometerStreamSample sample;
      while (manager.takeStreamSample(&sample)) consume(sample);
    }
  }

  const bool published = publishIfDue(now);
  if (!stream_owned_ && monotonic::reached(now, next_start_at_ms_)) {
    if (!manager.detectionComplete()) {
      next_start_at_ms_ = now + activity_auto_config::kBusyRetryMs;
    } else if (manager.faulted() || !manager.detected()) {
      // Absent/faulted hardware must not induce hot-loop I2C retries.
      incrementBounded(unavailable_attempts_);
      next_start_at_ms_ = now + activity_auto_config::kUnavailableRetryMs;
    } else if (manager.startStream()) {
      stream_owned_ = true;
    } else {
      // A USB diagnostic capture or the previous stream's shutdown still
      // owns the sensor. Retry cooperatively without disturbing it.
      next_start_at_ms_ = now + activity_auto_config::kBusyRetryMs;
    }
  }
  return published;
}

}  // namespace orun_tlp
