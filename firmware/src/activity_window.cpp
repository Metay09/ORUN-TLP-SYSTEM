#include "activity_window.h"

#include <stddef.h>

namespace orun_tlp {
namespace {

static_assert(activity_config::kWindowSampleCount > 1,
              "activity window requires at least two samples");
static_assert(activity_config::kMinimumInterSampleGapMs > 0,
              "activity timing minimum gap must be positive");
static_assert(activity_config::kMinimumInterSampleGapMs <=
                  activity_config::kExpectedSamplePeriodMs,
              "activity timing minimum gap must allow the expected period");
static_assert(activity_config::kMaximumInterSampleGapMs >=
                  activity_config::kExpectedSamplePeriodMs,
              "activity timing maximum gap must allow the expected period");

uint64_t squareSigned(int32_t value) {
  const int64_t widened = static_cast<int64_t>(value);
  return static_cast<uint64_t>(widened * widened);
}

uint32_t populationVariance(uint64_t sum_squared, int64_t sum,
                            uint32_t count) {
  if (count == 0) return 0;

  // variance = E[x^2] - E[x]^2. Compute as one rational expression so
  // truncating the integer mean first cannot create artificial variance.
  const uint64_t n = count;
  const uint64_t sum_magnitude =
      sum < 0 ? static_cast<uint64_t>(-sum) : static_cast<uint64_t>(sum);
  const uint64_t lhs = sum_squared * n;
  const uint64_t rhs = sum_magnitude * sum_magnitude;
  if (lhs <= rhs) return 0;
  return static_cast<uint32_t>((lhs - rhs) / (n * n));
}

uint32_t absoluteDifference(int16_t current, int16_t previous) {
  const int32_t delta = static_cast<int32_t>(current) -
                        static_cast<int32_t>(previous);
  return static_cast<uint32_t>(delta < 0 ? -delta : delta);
}

}  // namespace

ActivityWindow::ActivityWindow() { reset(); }

void ActivityWindow::reset() {
  sample_count_ = 0;
  timing_discontinuities_ = 0;
  have_previous_sample_ = false;
  first_captured_at_ms_ = 0;
  last_captured_at_ms_ = 0;
  previous_x_mg_ = 0;
  previous_y_mg_ = 0;
  previous_z_mg_ = 0;
  sum_x_mg_ = 0;
  sum_y_mg_ = 0;
  sum_z_mg_ = 0;
  sum_x_squared_mg2_ = 0;
  sum_y_squared_mg2_ = 0;
  sum_z_squared_mg2_ = 0;
  sum_magnitude_squared_mg2_ = 0;
  sum_abs_delta_mg_ = 0;
}

bool ActivityWindow::addSample(const AccelerometerSample& sample) {
  if (complete()) return false;

  if (!have_previous_sample_) {
    first_captured_at_ms_ = sample.captured_at_ms;
  } else {
    const uint32_t delta_ms = sample.captured_at_ms - last_captured_at_ms_;
    if (delta_ms < activity_config::kMinimumInterSampleGapMs ||
        delta_ms > activity_config::kMaximumInterSampleGapMs) {
      ++timing_discontinuities_;
    }

    sum_abs_delta_mg_ += absoluteDifference(sample.x_mg, previous_x_mg_);
    sum_abs_delta_mg_ += absoluteDifference(sample.y_mg, previous_y_mg_);
    sum_abs_delta_mg_ += absoluteDifference(sample.z_mg, previous_z_mg_);
  }

  sum_x_mg_ += sample.x_mg;
  sum_y_mg_ += sample.y_mg;
  sum_z_mg_ += sample.z_mg;

  const uint64_t x_squared = squareSigned(sample.x_mg);
  const uint64_t y_squared = squareSigned(sample.y_mg);
  const uint64_t z_squared = squareSigned(sample.z_mg);
  sum_x_squared_mg2_ += x_squared;
  sum_y_squared_mg2_ += y_squared;
  sum_z_squared_mg2_ += z_squared;
  sum_magnitude_squared_mg2_ += x_squared + y_squared + z_squared;

  previous_x_mg_ = sample.x_mg;
  previous_y_mg_ = sample.y_mg;
  previous_z_mg_ = sample.z_mg;
  last_captured_at_ms_ = sample.captured_at_ms;
  have_previous_sample_ = true;
  ++sample_count_;
  return true;
}

ActivityWindowFeatures ActivityWindow::features() const {
  ActivityWindowFeatures output;
  output.sample_count = sample_count_;
  output.complete = complete();
  output.timing_discontinuities = timing_discontinuities_;
  output.timing_continuous = complete() && timing_discontinuities_ == 0;

  if (sample_count_ == 0) return output;

  output.duration_ms = last_captured_at_ms_ - first_captured_at_ms_;
  output.mean_x_mg = static_cast<int32_t>(sum_x_mg_ / sample_count_);
  output.mean_y_mg = static_cast<int32_t>(sum_y_mg_ / sample_count_);
  output.mean_z_mg = static_cast<int32_t>(sum_z_mg_ / sample_count_);

  output.variance_x_mg2 =
      populationVariance(sum_x_squared_mg2_, sum_x_mg_, sample_count_);
  output.variance_y_mg2 =
      populationVariance(sum_y_squared_mg2_, sum_y_mg_, sample_count_);
  output.variance_z_mg2 =
      populationVariance(sum_z_squared_mg2_, sum_z_mg_, sample_count_);
  output.axis_variance_sum_mg2 = output.variance_x_mg2 +
                                 output.variance_y_mg2 +
                                 output.variance_z_mg2;

  output.mean_magnitude_squared_mg2 = static_cast<uint32_t>(
      sum_magnitude_squared_mg2_ / sample_count_);

  if (sample_count_ > 1) {
    output.mean_abs_delta_mg = static_cast<uint32_t>(
        sum_abs_delta_mg_ / static_cast<uint32_t>(sample_count_ - 1));
  }

  return output;
}

bool ActivityWindow::takeCompletedFeatures(ActivityWindowFeatures* output) {
  if (output == nullptr || !complete()) return false;
  *output = features();
  reset();
  return true;
}

}  // namespace orun_tlp
