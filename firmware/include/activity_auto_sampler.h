#pragma once

#include <stdint.h>

#include "activity_capture.h"

namespace orun_tlp {

// Opt-in, bounded RAM-only coordinator for the already-qualified RAK1904
// ActivityCapture path. It does not own I2C, RF, flash, power or permissions.
// One capture takes approximately five seconds; the sampling *opportunity*
// repeats at most every three minutes. No cow-behaviour classifier is implied.
namespace activity_auto_config {
constexpr uint32_t kCaptureIntervalMs = 3UL * 60UL * 1000UL;
constexpr uint32_t kSummaryPeriodMs = 60UL * 60UL * 1000UL;
static_assert(kCaptureIntervalMs < 0x80000000UL, "bounded capture interval");
static_assert(kSummaryPeriodMs < 0x80000000UL, "bounded summary period");
}  // namespace activity_auto_config

struct ActivityHourSummary {
  // The sampling period is independently anchored. It is NOT automatically
  // the current GNSS reporting period or an authoritative UTC time.
  uint32_t started_at_ms = 0;
  uint32_t finished_at_ms = 0;
  uint32_t duration_seconds = 0;  // actual elapsed, may exceed nominal 1 h.
  uint32_t measured_coverage_ms = 0;  // usable, nonoverlapping window duration.

  uint16_t usable_windows = 0;
  uint16_t invalid_windows = 0;
  uint16_t fault_windows = 0;
  uint16_t unavailable_attempts = 0;
  uint16_t boundary_discarded_windows = 0;
  uint32_t mean_axis_variance_sum_mg2 = 0;  // per usable window.
  uint32_t mean_abs_delta_mg = 0;          // per usable window.
};

// A service owner can later map this period summary into one typed product
// observation. No raw 10 Hz samples or per-capture records are stored here.
class ActivityAutoSampler {
 public:
  // Start immediately when explicitly enabled. Repeated ON is idempotent.
  // OFF prevents new captures; an already running ActivityCapture is allowed
  // to complete its existing bounded shutdown, rather than abandoning LIS3DH.
  void setEnabled(bool enabled, uint32_t now);
  bool enabled() const { return enabled_; }
  bool awaitingCapture() const { return awaiting_capture_; }

  // Call AFTER AccelerometerManager::poll() and ActivityCapture::poll().
  // Returns true exactly on publication of a new bounded RAM summary.
  bool poll(ActivityCapture& capture, uint32_t now);

  uint16_t usableWindows() const { return usable_windows_; }
  uint16_t invalidWindows() const { return invalid_windows_; }
  uint16_t faultWindows() const { return fault_windows_; }
  uint16_t unavailableAttempts() const { return unavailable_attempts_; }
  uint32_t nextDueAtMs() const { return next_due_at_ms_; }

  bool summaryReady() const { return summary_ready_; }
  const ActivityHourSummary& latestSummary() const { return latest_summary_; }
  uint32_t publishedSummaries() const { return published_summaries_; }

 private:
  void recordCompleted(const ActivityCapture& capture);
  bool publishIfDue(uint32_t now);

  bool enabled_ = false;
  bool awaiting_capture_ = false;
  bool summary_ready_ = false;
  uint32_t period_started_at_ms_ = 0;
  uint32_t next_due_at_ms_ = 0;
  uint32_t capture_started_at_ms_ = 0;
  uint64_t summed_coverage_ms_ = 0;
  uint16_t usable_windows_ = 0;
  uint16_t invalid_windows_ = 0;
  uint16_t fault_windows_ = 0;
  uint16_t unavailable_attempts_ = 0;
  uint16_t boundary_discarded_windows_ = 0;
  uint64_t summed_variance_mg2_ = 0;
  uint64_t summed_abs_delta_mg_ = 0;
  uint32_t published_summaries_ = 0;
  ActivityHourSummary latest_summary_{};
};

}  // namespace orun_tlp
