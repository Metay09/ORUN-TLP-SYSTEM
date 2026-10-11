#pragma once

#include <stdint.h>

#include "accelerometer_manager.h"
#include "activity_classifier.h"
#include "activity_window.h"

namespace orun_tlp {

// Opt-in, bounded RAM-only consumer of the AccelerometerManager continuous
// 10 Hz FIFO stream (owner decision 2026-10-12, replacing 5 s every 3 min).
// It does not own I2C, RF, flash, power or permissions. The stream is cut
// into back-to-back 5-second windows; each usable window is classified
// active/inactive by the research-derived, unvalidated threshold of
// activity_classifier.h; no grazing/walking/lying label is implied. A
// continuity break (FIFO overrun, failed drain, stream restart) drops the
// partial window, so lost time stays unmeasured and is never filled in.
namespace activity_auto_config {
// Absent or faulted accelerometer: how often the sampler looks again.
constexpr uint32_t kUnavailableRetryMs = 3UL * 60UL * 1000UL;
// Sensor detection pending, or a USB diagnostic capture owns the sensor.
constexpr uint32_t kBusyRetryMs = 1000UL;
constexpr uint32_t kSummaryPeriodMs = 60UL * 60UL * 1000UL;
// A usable window is 50 FIFO samples taken back to back at the sensor's
// output data rate, so it covers 50 sample periods (5 s) of time.
constexpr uint32_t kWindowCoverageMs =
    activity_config::kWindowSampleCount * activity_config::kExpectedSamplePeriodMs;
static_assert(kUnavailableRetryMs < 0x80000000UL, "bounded retry interval");
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
  uint16_t invalid_windows = 0;  // partial windows lost to a continuity break
  uint16_t fault_windows = 0;    // streams ended by a sensor fault
  uint16_t unavailable_attempts = 0;
  uint16_t boundary_discarded_windows = 0;
  uint32_t mean_axis_variance_sum_mg2 = 0;  // per usable window.
  uint32_t mean_abs_delta_mg = 0;          // per usable window.
  // activity_classifier.h: usable windows classified active, their measured
  // duration, and active<->inactive changes between consecutive usable
  // windows of this period (the first window is not a transition).
  uint16_t active_windows = 0;
  uint16_t activity_transitions = 0;
  uint32_t measured_active_ms = 0;
  // activity_classifier.h head position: part of measured_active_ms spent
  // with the head down (the published grazing rule). Only meaningful when
  // head_pose_classified: the neck axis was known for every active window of
  // the period.
  uint32_t measured_head_down_active_ms = 0;
  bool head_pose_classified = false;
};

// A service owner can later map this period summary into one typed product
// observation. No raw 10 Hz samples or per-window records are stored here.
class ActivityAutoSampler {
 public:
  // Start immediately when explicitly enabled. Repeated ON is idempotent.
  // OFF stops the stream at the next poll through the manager's bounded
  // power-down, rather than abandoning LIS3DH in 10 Hz mode.
  void setEnabled(bool enabled, uint32_t now);
  bool enabled() const { return enabled_; }
  bool streaming() const { return stream_owned_; }

  // Call AFTER AccelerometerManager::poll(), every loop pass, also while
  // disabled (to stop the stream). Starts the stream when possible and takes
  // every drained sample. Returns true exactly on an automatic hourly
  // publication (not report-owned).
  bool poll(AccelerometerManager& manager, uint32_t now);

  // Report-owned periods (SF5 PERIODIC): the report owner closes each period
  // at its own boundary with closePeriod(), so the summary anchors are exactly
  // the report period's and link with linkActivityPeriodEvidence(). While
  // report-owned there is no automatic hourly publication. A window still
  // filling at the close is discarded, never split or moved into the next
  // period; samples taken before a period started never enter it.
  void setReportOwned(bool report_owned) { report_owned_ = report_owned; }
  bool reportOwned() const { return report_owned_; }
  // Publishes [period start, now] and starts the next period at `now`.
  // False when disabled or the period is shorter than one second.
  bool closePeriod(uint32_t now);

  uint16_t usableWindows() const { return usable_windows_; }
  uint16_t invalidWindows() const { return invalid_windows_; }
  uint16_t faultWindows() const { return fault_windows_; }
  uint16_t unavailableAttempts() const { return unavailable_attempts_; }
  uint16_t windowSampleCount() const { return window_.sampleCount(); }

  // Which accelerometer axis runs along the neck toward the head (see
  // activity_classifier.h). kUnset leaves head position unknown. A change
  // takes effect at the next window; the current period's head position is
  // reported only if the axis was set for all of its active windows.
  void setNeckForwardAxis(NeckAxis axis) { neck_axis_ = axis; }
  NeckAxis neckForwardAxis() const { return neck_axis_; }
  HeadPose headPose() const { return head_pose_; }
  // Diagnostics: mean acceleration of the last usable window, milli-g.
  int32_t lastMeanXMg() const { return last_mean_x_mg_; }
  int32_t lastMeanYMg() const { return last_mean_y_mg_; }
  int32_t lastMeanZMg() const { return last_mean_z_mg_; }
  // Diagnostics only: mean |a|^2 of the last usable window. Near 1 g^2
  // (1,000,000 mg^2) when the data path is scaled correctly.
  uint32_t lastMeanMagnitudeSquaredMg2() const { return last_magnitude_mg2_; }

  bool summaryReady() const { return summary_ready_; }
  const ActivityHourSummary& latestSummary() const { return latest_summary_; }
  uint32_t publishedSummaries() const { return published_summaries_; }

 private:
  void consume(const AccelerometerStreamSample& sample);
  void recordWindow(const ActivityWindowFeatures& features);
  bool publishIfDue(uint32_t now);
  void publish(uint32_t now);
  void resetPeriod(uint32_t now);

  bool enabled_ = false;
  bool stream_owned_ = false;
  bool summary_ready_ = false;
  uint32_t period_started_at_ms_ = 0;
  uint32_t next_start_at_ms_ = 0;
  ActivityWindow window_{};
  uint32_t window_first_ms_ = 0;
  uint64_t summed_coverage_ms_ = 0;
  uint16_t usable_windows_ = 0;
  uint16_t invalid_windows_ = 0;
  uint16_t fault_windows_ = 0;
  uint16_t unavailable_attempts_ = 0;
  uint16_t boundary_discarded_windows_ = 0;
  uint64_t summed_variance_mg2_ = 0;
  uint64_t summed_abs_delta_mg_ = 0;
  uint64_t summed_active_ms_ = 0;
  uint16_t active_windows_ = 0;
  uint16_t transitions_ = 0;
  bool has_last_window_ = false;
  bool last_window_active_ = false;
  bool report_owned_ = false;
  uint32_t last_magnitude_mg2_ = 0;
  NeckAxis neck_axis_ = activity_classifier::kDefaultNeckForwardAxis;
  HeadPose head_pose_ = HeadPose::kUp;
  uint64_t summed_head_down_active_ms_ = 0;
  uint16_t unclassified_active_windows_ = 0;
  int32_t last_mean_x_mg_ = 0;
  int32_t last_mean_y_mg_ = 0;
  int32_t last_mean_z_mg_ = 0;
  uint32_t published_summaries_ = 0;
  ActivityHourSummary latest_summary_{};
};

}  // namespace orun_tlp
