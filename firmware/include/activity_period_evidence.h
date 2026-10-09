#pragma once

#include <stdint.h>

#include "activity_auto_sampler.h"

namespace orun_tlp {

// SF5A period-level *evidence* only, NOT a serialized SF5B ACTIVITY_VALID field.
// The existing RAK1904 windows have no validated active/inactive classifier.
// Therefore their physical movement statistics must never imply active_seconds=0
// (which would falsely label every usable sample as inactive).
enum class ActivityPeriodLinkResult : uint8_t {
  kLinkedUnclassified,  // real sampled duration, but NO active/inactive label.
  kNoUsableEvidence,
  kInvalidPeriod,
  kPeriodMismatch,
};

struct ActivityPeriodEvidence {
  uint32_t period_duration_seconds = 0;
  uint32_t coverage_seconds = 0;
  uint32_t unknown_seconds = 0;
  uint16_t usable_windows = 0;
  uint16_t boundary_discarded_windows = 0;
  uint16_t movement_mean_abs_delta_mg = 0;  // saturates at 0xFFFF.
  bool movement_evidence_present = false;
  bool active_inactive_classification_valid = false; // always false for M6.
};

// A consumer must provide the actual period anchors from its record owner,
// not just a similarly sized interval. With no established matching anchors,
// refuse linkage: a 1-hour activity summary is NOT the 3-minute GNSS report.
// Millisecond timestamps are monotonic, never epoch/UTC.
ActivityPeriodLinkResult linkActivityPeriodEvidence(
    const ActivityHourSummary& summary,
    uint32_t report_started_at_ms, uint32_t report_finished_at_ms,
    ActivityPeriodEvidence& output);

}  // namespace orun_tlp
