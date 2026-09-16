#include "activity_quality.h"

namespace orun_tlp {

ActivityWindowAssessment assessActivityWindow(
    const ActivityWindowFeatures& features) {
  if (!features.complete ||
      features.sample_count != activity_config::kWindowSampleCount) {
    return ActivityWindowAssessment(false,
                                    ActivityWindowEligibility::kIncomplete);
  }

  if (!features.timing_continuous ||
      features.timing_discontinuities != 0) {
    return ActivityWindowAssessment(false,
                                    ActivityWindowEligibility::kTimingInvalid);
  }

  // A structurally complete window has N-1 adjacent timing intervals. Reject
  // externally constructed or corrupted feature objects whose aggregate
  // duration contradicts the same interval bounds used by ActivityWindow.
  // This remains a timing-quality check, not a behavior-classification rule.
  const uint32_t interval_count =
      static_cast<uint32_t>(activity_config::kWindowSampleCount - 1U);
  const uint32_t minimum_duration_ms =
      interval_count * activity_config::kMinimumInterSampleGapMs;
  const uint32_t maximum_duration_ms =
      interval_count * activity_config::kMaximumInterSampleGapMs;
  if (features.duration_ms < minimum_duration_ms ||
      features.duration_ms > maximum_duration_ms) {
    return ActivityWindowAssessment(false,
                                    ActivityWindowEligibility::kTimingInvalid);
  }

  return ActivityWindowAssessment(true, ActivityWindowEligibility::kUsable);
}

}  // namespace orun_tlp
