#pragma once

#include <stdint.h>

#include "activity_window.h"

namespace orun_tlp {

enum class ActivityWindowEligibility : uint8_t {
  kUsable,
  kIncomplete,
  kTimingInvalid,
};

struct ActivityWindowAssessment {
  constexpr ActivityWindowAssessment(
      bool usable_value = false,
      ActivityWindowEligibility reason_value =
          ActivityWindowEligibility::kIncomplete)
      : usable(usable_value), reason(reason_value) {}

  bool usable;
  ActivityWindowEligibility reason;
};

// Pure fail-closed gate between deterministic feature extraction and any later
// behavior classifier. This validates structural/timing eligibility only; it
// deliberately applies no cattle-behavior or motion thresholds.
ActivityWindowAssessment assessActivityWindow(
    const ActivityWindowFeatures& features);

}  // namespace orun_tlp
