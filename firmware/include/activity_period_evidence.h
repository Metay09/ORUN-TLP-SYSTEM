#pragma once

#include <stdint.h>

#include "activity_auto_sampler.h"

namespace orun_tlp {

// Period-level activity evidence for one SF5 PERIODIC record. Since
// 2026-10-11 each usable window carries the research-derived (unvalidated)
// active/inactive label of activity_classifier.h, so linked evidence includes
// active seconds and transitions. It is still not a grazing/walking/lying
// label, and an absent label must never be reported as active_seconds=0.
enum class ActivityPeriodLinkResult : uint8_t {
  kLinkedUnclassified,  // real sampled duration, but NO active/inactive label.
  kNoUsableEvidence,
  kInvalidPeriod,
  kPeriodMismatch,
  kLinkedClassified,    // sampled duration with active seconds/transitions.
};

struct ActivityPeriodEvidence {
  uint32_t period_duration_seconds = 0;
  uint32_t coverage_seconds = 0;
  uint32_t unknown_seconds = 0;
  uint16_t usable_windows = 0;
  uint16_t boundary_discarded_windows = 0;
  uint16_t movement_mean_abs_delta_mg = 0;  // saturates at 0xFFFF.
  uint32_t active_seconds = 0;              // <= coverage_seconds.
  uint16_t transition_count = 0;            // saturates at 0xFFFF.
  bool movement_evidence_present = false;
  bool active_inactive_classification_valid = false;
  // Share of the active time with the head down (published grazing rule),
  // in 0.5 % steps: 0..kHeadDownShareMax. Only with head_pose_valid.
  uint8_t head_down_share = 0;
  bool head_pose_valid = false;
};

constexpr uint8_t kHeadDownShareMax = 200;  // 100 %

// A consumer must provide the actual period anchors from its record owner,
// not just a similarly sized interval. With no established matching anchors,
// refuse linkage: a 1-hour activity summary is NOT the 3-minute GNSS report.
// Millisecond timestamps are monotonic, never epoch/UTC.
ActivityPeriodLinkResult linkActivityPeriodEvidence(
    const ActivityHourSummary& summary,
    uint32_t report_started_at_ms, uint32_t report_finished_at_ms,
    ActivityPeriodEvidence& output);

}  // namespace orun_tlp
