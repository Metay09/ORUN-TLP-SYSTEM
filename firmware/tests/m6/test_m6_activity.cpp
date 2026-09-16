#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>

#include "activity_window.h"

using namespace orun_tlp;

namespace {

void addConstantWindow(ActivityWindow& window, uint32_t start_ms,
                       int16_t x_mg, int16_t y_mg, int16_t z_mg) {
  for (uint16_t i = 0; i < activity_config::kWindowSampleCount; ++i) {
    const uint32_t captured_at =
        start_ms + static_cast<uint32_t>(i) *
                       activity_config::kExpectedSamplePeriodMs;
    assert(window.addSample(
        AccelerometerSample(captured_at, x_mg, y_mg, z_mg)));
  }
}

void emptyAndIncompleteWindowsDoNotPublish() {
  ActivityWindow window;
  ActivityWindowFeatures features = window.features();
  assert(features.sample_count == 0);
  assert(!features.complete);
  assert(!features.timing_continuous);
  assert(!window.takeCompletedFeatures(&features));

  for (uint16_t i = 0; i < activity_config::kWindowSampleCount - 1; ++i) {
    assert(window.addSample(AccelerometerSample(
        static_cast<uint32_t>(i) * 100U, 0, 0, 1000)));
  }
  assert(window.sampleCount() == activity_config::kWindowSampleCount - 1);
  assert(!window.complete());
  assert(!window.takeCompletedFeatures(&features));
  assert(window.sampleCount() == activity_config::kWindowSampleCount - 1);
}

void staticGravityProducesZeroMotionFeatures() {
  ActivityWindow window;
  addConstantWindow(window, 1000, 0, 0, 1000);

  const ActivityWindowFeatures features = window.features();
  assert(features.sample_count == activity_config::kWindowSampleCount);
  assert(features.complete);
  assert(features.timing_continuous);
  assert(features.timing_discontinuities == 0);
  assert(features.duration_ms == 4900);
  assert(features.mean_x_mg == 0);
  assert(features.mean_y_mg == 0);
  assert(features.mean_z_mg == 1000);
  assert(features.variance_x_mg2 == 0);
  assert(features.variance_y_mg2 == 0);
  assert(features.variance_z_mg2 == 0);
  assert(features.axis_variance_sum_mg2 == 0);
  assert(features.mean_magnitude_squared_mg2 == 1000000U);
  assert(features.mean_abs_delta_mg == 0);

  // A full window is immutable until explicitly taken/reset.
  assert(!window.addSample(AccelerometerSample(7000, 1, 2, 3)));
  assert(window.features().mean_z_mg == 1000);
}

void magnitudeFeatureDoesNotDependOnBoardAxis() {
  ActivityWindow z_axis;
  ActivityWindow x_axis;
  addConstantWindow(z_axis, 0, 0, 0, 1000);
  addConstantWindow(x_axis, 0, 1000, 0, 0);

  const ActivityWindowFeatures z = z_axis.features();
  const ActivityWindowFeatures x = x_axis.features();
  assert(z.mean_magnitude_squared_mg2 == x.mean_magnitude_squared_mg2);
  assert(z.mean_magnitude_squared_mg2 == 1000000U);
  assert(z.axis_variance_sum_mg2 == 0);
  assert(x.axis_variance_sum_mg2 == 0);
}

void alternatingMotionProducesVarianceAndDelta() {
  ActivityWindow window;
  for (uint16_t i = 0; i < activity_config::kWindowSampleCount; ++i) {
    const int16_t x = (i & 1U) == 0 ? 100 : -100;
    assert(window.addSample(AccelerometerSample(
        static_cast<uint32_t>(i) * 100U, x, 0, 1000)));
  }

  const ActivityWindowFeatures features = window.features();
  assert(features.complete);
  assert(features.timing_continuous);
  assert(features.mean_x_mg == 0);
  assert(features.mean_z_mg == 1000);
  assert(features.variance_x_mg2 == 10000U);
  assert(features.variance_y_mg2 == 0);
  assert(features.variance_z_mg2 == 0);
  assert(features.axis_variance_sum_mg2 == 10000U);
  assert(features.mean_magnitude_squared_mg2 == 1010000U);
  assert(features.mean_abs_delta_mg == 200U);
}

void timingGapMarksWindowUnreliableWithoutDroppingSamples() {
  ActivityWindow window;
  for (uint16_t i = 0; i < activity_config::kWindowSampleCount; ++i) {
    uint32_t captured_at = static_cast<uint32_t>(i) * 100U;
    if (i >= 11) captured_at += 300U;  // One 400 ms gap at sample 11.
    assert(window.addSample(
        AccelerometerSample(captured_at, 0, 0, 1000)));
  }

  const ActivityWindowFeatures features = window.features();
  assert(features.complete);
  assert(!features.timing_continuous);
  assert(features.timing_discontinuities == 1);
  assert(features.sample_count == activity_config::kWindowSampleCount);
  assert(features.duration_ms == 5200U);
  assert(features.mean_magnitude_squared_mg2 == 1000000U);
}

void tooFastTimingMarksWindowUnreliable() {
  ActivityWindow window;
  for (uint16_t i = 0; i < activity_config::kWindowSampleCount; ++i) {
    uint32_t captured_at = static_cast<uint32_t>(i) * 100U;
    // Shift all samples from index 11 onward so exactly one adjacent interval
    // becomes 25 ms; subsequent intervals return to the nominal 100 ms.
    if (i >= 11) captured_at -= 75U;
    assert(window.addSample(
        AccelerometerSample(captured_at, 0, 0, 1000)));
  }

  const ActivityWindowFeatures features = window.features();
  assert(features.complete);
  assert(!features.timing_continuous);
  assert(features.timing_discontinuities == 1);
  assert(features.sample_count == activity_config::kWindowSampleCount);
  assert(features.duration_ms == 4825U);
}

void timestampsRemainValidAcrossMillisRollover() {
  ActivityWindow window;
  const uint32_t start = UINT32_MAX - 200U;
  addConstantWindow(window, start, 0, 0, 1000);

  const ActivityWindowFeatures features = window.features();
  assert(features.complete);
  assert(features.timing_continuous);
  assert(features.timing_discontinuities == 0);
  assert(features.duration_ms == 4900U);
}

void extremeInputUsesWideAccumulators() {
  ActivityWindow window;
  for (uint16_t i = 0; i < activity_config::kWindowSampleCount; ++i) {
    const int16_t value = (i & 1U) == 0 ? INT16_MAX : INT16_MIN;
    assert(window.addSample(AccelerometerSample(
        static_cast<uint32_t>(i) * 100U, value, value, value)));
  }

  const ActivityWindowFeatures features = window.features();
  assert(features.complete);
  assert(features.timing_continuous);
  assert(features.variance_x_mg2 > 1000000000U);
  assert(features.variance_y_mg2 == features.variance_x_mg2);
  assert(features.variance_z_mg2 == features.variance_x_mg2);
  assert(features.axis_variance_sum_mg2 > 3000000000U);
  assert(features.mean_magnitude_squared_mg2 > 3000000000U);
  assert(features.mean_abs_delta_mg == 196605U);
}

void takingCompletedWindowResetsOwnershipCleanly() {
  ActivityWindow window;
  addConstantWindow(window, 0, 0, 0, 1000);

  ActivityWindowFeatures output;
  assert(!window.takeCompletedFeatures(nullptr));
  assert(window.complete());
  assert(window.takeCompletedFeatures(&output));
  assert(output.complete);
  assert(output.sample_count == activity_config::kWindowSampleCount);
  assert(window.sampleCount() == 0);
  assert(!window.complete());

  // A new window cannot inherit any previous sums or timing state.
  addConstantWindow(window, 10000, 1000, 0, 0);
  const ActivityWindowFeatures next = window.features();
  assert(next.mean_x_mg == 1000);
  assert(next.mean_z_mg == 0);
  assert(next.axis_variance_sum_mg2 == 0);
  assert(next.mean_abs_delta_mg == 0);
  assert(next.timing_continuous);
}

}  // namespace

int main() {
  emptyAndIncompleteWindowsDoNotPublish();
  staticGravityProducesZeroMotionFeatures();
  magnitudeFeatureDoesNotDependOnBoardAxis();
  alternatingMotionProducesVarianceAndDelta();
  timingGapMarksWindowUnreliableWithoutDroppingSamples();
  tooFastTimingMarksWindowUnreliable();
  timestampsRemainValidAcrossMillisRollover();
  extremeInputUsesWideAccumulators();
  takingCompletedWindowResetsOwnershipCleanly();
  puts("M6B fixed-memory activity window/feature checks: PASS");
}
