#pragma once

#include <stdint.h>

#include "accelerometer_sample.h"

namespace orun_tlp {
namespace activity_config {

// Initial host-only feature window. This is an implementation seed, not a
// validated cattle-classification window. Field data may revise it before the
// classifier is integrated into production runtime.
constexpr uint16_t kWindowSampleCount = 50;  // 5 s at the current 10 Hz target.
constexpr uint32_t kExpectedSamplePeriodMs = 100;
constexpr uint32_t kMaximumInterSampleGapMs = 300;

}  // namespace activity_config

struct ActivityWindowFeatures {
  constexpr ActivityWindowFeatures()
      : sample_count(0),
        duration_ms(0),
        complete(false),
        timing_continuous(false),
        timing_discontinuities(0),
        mean_x_mg(0),
        mean_y_mg(0),
        mean_z_mg(0),
        variance_x_mg2(0),
        variance_y_mg2(0),
        variance_z_mg2(0),
        axis_variance_sum_mg2(0),
        mean_magnitude_squared_mg2(0),
        mean_abs_delta_mg(0) {}

  uint16_t sample_count;
  uint32_t duration_ms;
  bool complete;
  bool timing_continuous;
  uint16_t timing_discontinuities;

  int32_t mean_x_mg;
  int32_t mean_y_mg;
  int32_t mean_z_mg;

  uint32_t variance_x_mg2;
  uint32_t variance_y_mg2;
  uint32_t variance_z_mg2;
  uint32_t axis_variance_sum_mg2;

  // Orientation-independent gravity/motion energy proxy. No square root or
  // floating point is required on the tracker.
  uint32_t mean_magnitude_squared_mg2;

  // Mean Manhattan delta between adjacent samples:
  // (|dx| + |dy| + |dz|) / interval_count.
  uint32_t mean_abs_delta_mg;
};

// Fixed-memory streaming feature accumulator. It intentionally owns no sensor
// I/O, role/config state, RF encoding or persistence. M6B1 is host-only and
// consumes already-captured physical observations from the M6A boundary.
class ActivityWindow {
 public:
  ActivityWindow();

  void reset();

  // Returns false once the current fixed window is full. Samples are never
  // overwritten implicitly; callers must take/reset the completed window.
  bool addSample(const AccelerometerSample& sample);

  bool complete() const {
    return sample_count_ == activity_config::kWindowSampleCount;
  }
  uint16_t sampleCount() const { return sample_count_; }

  ActivityWindowFeatures features() const;

  // Emits only a complete window and resets the accumulator on success.
  bool takeCompletedFeatures(ActivityWindowFeatures* output);

 private:
  uint16_t sample_count_ = 0;
  uint16_t timing_discontinuities_ = 0;

  bool have_previous_sample_ = false;
  uint32_t first_captured_at_ms_ = 0;
  uint32_t last_captured_at_ms_ = 0;
  int16_t previous_x_mg_ = 0;
  int16_t previous_y_mg_ = 0;
  int16_t previous_z_mg_ = 0;

  int64_t sum_x_mg_ = 0;
  int64_t sum_y_mg_ = 0;
  int64_t sum_z_mg_ = 0;
  uint64_t sum_x_squared_mg2_ = 0;
  uint64_t sum_y_squared_mg2_ = 0;
  uint64_t sum_z_squared_mg2_ = 0;
  uint64_t sum_magnitude_squared_mg2_ = 0;
  uint64_t sum_abs_delta_mg_ = 0;
};

}  // namespace orun_tlp
