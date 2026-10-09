#include "activity_auto_sampler.h"

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
  awaiting_capture_ = false;
  if (!enabled_) return;  // Do not interrupt the manager's sensor shutdown.
  period_started_at_ms_ = now;
  next_due_at_ms_ = now;
  usable_windows_ = invalid_windows_ = fault_windows_ = 0;
  unavailable_attempts_ = boundary_discarded_windows_ = 0;
  summed_variance_mg2_ = summed_abs_delta_mg_ = 0;
  summed_coverage_ms_ = 0;
  summary_ready_ = false;
  latest_summary_ = ActivityHourSummary{};
}

void ActivityAutoSampler::recordCompleted(const ActivityCapture& capture) {
  const ActivityCapture::State state = capture.state();
  if (state == ActivityCapture::State::kFault) {
    incrementBounded(fault_windows_);
    return;
  }
  if (state != ActivityCapture::State::kReady ||
      !capture.assessment().usable || capture.result() == nullptr) {
    incrementBounded(invalid_windows_);
    return;
  }
  incrementBounded(usable_windows_);
  // Actual first-to-last sample interval: do not pretend 50 samples cover
  // 180 seconds, nor count initial sensor setup/settling as observations.
  summed_coverage_ms_ += capture.result()->duration_ms;
  summed_variance_mg2_ += capture.result()->axis_variance_sum_mg2;
  summed_abs_delta_mg_ += capture.result()->mean_abs_delta_mg;
}

bool ActivityAutoSampler::publishIfDue(uint32_t now) {
  if (!monotonic::elapsed(now, period_started_at_ms_,
                          activity_auto_config::kSummaryPeriodMs)) {
    return false;
  }
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
  latest_summary_ = summary;
  summary_ready_ = true;
  if (published_summaries_ != UINT32_MAX) ++published_summaries_;
  period_started_at_ms_ = now;  // No catch-up burst or fabricated lost periods.
  usable_windows_ = invalid_windows_ = fault_windows_ = 0;
  unavailable_attempts_ = boundary_discarded_windows_ = 0;
  summed_variance_mg2_ = summed_abs_delta_mg_ = 0;
  summed_coverage_ms_ = 0;
  return true;
}

bool ActivityAutoSampler::poll(ActivityCapture& capture, uint32_t now) {
  if (!enabled_) return false;

  if (awaiting_capture_) {
    const auto state = capture.state();
    if (state == ActivityCapture::State::kReady ||
        state == ActivityCapture::State::kInvalid ||
        state == ActivityCapture::State::kFault) {
      // Never allocate a short window to an hour that it straddles.
      // Late completion after a previous report was published must not
      // be misattributed to the new reporting period either.
      const bool started_in_period =
          monotonic::reached(capture_started_at_ms_, period_started_at_ms_);
      const bool finished_before_deadline =
          !monotonic::elapsed(now, period_started_at_ms_,
                              activity_auto_config::kSummaryPeriodMs);
      if (started_in_period && finished_before_deadline) {
        recordCompleted(capture);
      } else {
        incrementBounded(boundary_discarded_windows_);
      }
      awaiting_capture_ = false;
      next_due_at_ms_ = now + activity_auto_config::kCaptureIntervalMs;
    }
  }

  const bool published = publishIfDue(now);
  if (!awaiting_capture_ && monotonic::reached(now, next_due_at_ms_)) {
    const ActivityCapture::StartResult result = capture.start();
    if (result == ActivityCapture::StartResult::kStarted) {
      awaiting_capture_ = true;
      capture_started_at_ms_ = now;
    } else if (result == ActivityCapture::StartResult::kPending ||
               result == ActivityCapture::StartResult::kBusy) {
      // Capability detection or an explicit USB capture still owns the sensor.
      // Retry cooperatively, without overwriting that result/session.
      next_due_at_ms_ = now + 1000U;
    } else {
      // Absent/faulted hardware must not induce hot-loop I2C retries.
      incrementBounded(unavailable_attempts_);
      next_due_at_ms_ = now + activity_auto_config::kCaptureIntervalMs;
    }
  }
  return published;
}

}  // namespace orun_tlp
