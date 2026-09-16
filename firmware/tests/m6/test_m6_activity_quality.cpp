#include <assert.h>
#include <stdio.h>

#include "activity_quality.h"

using namespace orun_tlp;

namespace {

ActivityWindowFeatures usableFeatures() {
  ActivityWindowFeatures features;
  features.sample_count = activity_config::kWindowSampleCount;
  features.complete = true;
  features.timing_continuous = true;
  features.timing_discontinuities = 0;
  return features;
}

void completeContinuousWindowIsUsable() {
  const ActivityWindowAssessment assessment =
      assessActivityWindow(usableFeatures());
  assert(assessment.usable);
  assert(assessment.reason == ActivityWindowEligibility::kUsable);
}

void incompleteWindowIsRejected() {
  ActivityWindowFeatures features = usableFeatures();
  features.complete = false;
  const ActivityWindowAssessment assessment = assessActivityWindow(features);
  assert(!assessment.usable);
  assert(assessment.reason == ActivityWindowEligibility::kIncomplete);
}

void sampleCountMismatchFailsClosed() {
  ActivityWindowFeatures features = usableFeatures();
  features.sample_count = activity_config::kWindowSampleCount - 1;
  const ActivityWindowAssessment assessment = assessActivityWindow(features);
  assert(!assessment.usable);
  assert(assessment.reason == ActivityWindowEligibility::kIncomplete);
}

void timingDiscontinuityIsRejected() {
  ActivityWindowFeatures features = usableFeatures();
  features.timing_continuous = false;
  features.timing_discontinuities = 1;
  const ActivityWindowAssessment assessment = assessActivityWindow(features);
  assert(!assessment.usable);
  assert(assessment.reason == ActivityWindowEligibility::kTimingInvalid);
}

void contradictoryTimingFieldsFailClosed() {
  ActivityWindowFeatures features = usableFeatures();
  features.timing_discontinuities = 1;
  const ActivityWindowAssessment discontinuity =
      assessActivityWindow(features);
  assert(!discontinuity.usable);
  assert(discontinuity.reason == ActivityWindowEligibility::kTimingInvalid);

  features = usableFeatures();
  features.timing_continuous = false;
  features.timing_discontinuities = 0;
  const ActivityWindowAssessment false_continuous =
      assessActivityWindow(features);
  assert(!false_continuous.usable);
  assert(false_continuous.reason == ActivityWindowEligibility::kTimingInvalid);
}

void motionValuesDoNotAffectEligibility() {
  ActivityWindowFeatures features = usableFeatures();
  features.axis_variance_sum_mg2 = 0;
  features.mean_magnitude_squared_mg2 = 0;
  features.mean_abs_delta_mg = 0;
  assert(assessActivityWindow(features).usable);

  features.axis_variance_sum_mg2 = 4000000000U;
  features.mean_magnitude_squared_mg2 = 4000000000U;
  features.mean_abs_delta_mg = 100000U;
  assert(assessActivityWindow(features).usable);
}

}  // namespace

int main() {
  completeContinuousWindowIsUsable();
  incompleteWindowIsRejected();
  sampleCountMismatchFailsClosed();
  timingDiscontinuityIsRejected();
  contradictoryTimingFieldsFailClosed();
  motionValuesDoNotAffectEligibility();
  puts("M6B activity feature eligibility checks: PASS");
}
