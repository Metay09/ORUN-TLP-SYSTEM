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

  return ActivityWindowAssessment(true, ActivityWindowEligibility::kUsable);
}

}  // namespace orun_tlp
