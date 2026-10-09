#include "activity_period_evidence.h"

namespace orun_tlp {

ActivityPeriodLinkResult linkActivityPeriodEvidence(
    const ActivityHourSummary& summary,
    uint32_t report_started_at_ms, uint32_t report_finished_at_ms,
    ActivityPeriodEvidence& output) {
  output = ActivityPeriodEvidence{};
  const uint32_t elapsed_ms =
      report_finished_at_ms - report_started_at_ms;
  // Need one nonzero, bounded report period. Modulo wrap is supported.
  if (elapsed_ms < 1000U || elapsed_ms >= 0x80000000UL)
    return ActivityPeriodLinkResult::kInvalidPeriod;

  // A period with the right *duration* but different anchors is not the
  // same record. Never silently splice an hourly window into a 3-minute
  // tracking record or shift evidence across a reset/reconfiguration gap.
  if (report_started_at_ms != summary.started_at_ms ||
      report_finished_at_ms != summary.finished_at_ms)
    return ActivityPeriodLinkResult::kPeriodMismatch;
  const uint32_t period_seconds = elapsed_ms / 1000U;
  if (period_seconds != summary.duration_seconds ||
      summary.measured_coverage_ms > elapsed_ms ||
      (summary.usable_windows == 0U && summary.measured_coverage_ms != 0U))
    return ActivityPeriodLinkResult::kInvalidPeriod;

  output.period_duration_seconds = period_seconds;
  output.coverage_seconds = summary.measured_coverage_ms / 1000U;
  if (output.coverage_seconds > period_seconds) {
    output = ActivityPeriodEvidence{};
    return ActivityPeriodLinkResult::kInvalidPeriod;
  }
  output.unknown_seconds = period_seconds - output.coverage_seconds;
  output.usable_windows = summary.usable_windows;
  output.boundary_discarded_windows = summary.boundary_discarded_windows;
  if (summary.usable_windows == 0U || output.coverage_seconds == 0U)
    return ActivityPeriodLinkResult::kNoUsableEvidence;

  output.movement_mean_abs_delta_mg =
      summary.mean_abs_delta_mg > UINT16_MAX
          ? UINT16_MAX
          : static_cast<uint16_t>(summary.mean_abs_delta_mg);
  output.movement_evidence_present = true;
  // Intentionally no ACTIVE/INACTIVE duration. This type is a pre-wire
  // evidence boundary. SF5B schema v1 demands ACTIVITY_VALID=0 and canonical
  // zero activity wire fields until validated active_seconds are available.
  return ActivityPeriodLinkResult::kLinkedUnclassified;
}

}  // namespace orun_tlp
