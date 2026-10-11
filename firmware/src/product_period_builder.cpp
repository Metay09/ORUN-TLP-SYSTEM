#include "product_period_builder.h"

namespace orun_tlp {
namespace {

uint8_t saturate8(uint32_t value) {
  return value > 255U ? 255U : static_cast<uint8_t>(value);
}

}  // namespace

tlp::ProductSecureCodecResult buildPeriodicPlaintext(
    const PeriodicRecordInputs& in, tlp::PeriodicObservationPlaintext* output) {
  if (output == nullptr) return tlp::ProductSecureCodecResult::kInvalidArgument;
  tlp::PeriodicObservationPlaintext p;
  uint8_t flags = 0;
  p.record_incarnation = in.record_incarnation;
  p.record_sequence = in.record_sequence;
  p.period_duration_seconds = in.period_duration_seconds;

  if (in.period_end_time_valid && in.period_end_epoch_seconds != 0) {
    flags |= tlp::kPeriodicPeriodTimeValid;
    p.period_end_epoch_seconds = in.period_end_epoch_seconds;
  }

  const PeriodicLocationInput& location = in.location;
  if (location.valid) {
    flags |= tlp::kPeriodicLocationValid;
    p.location_source = tlp::kLocationSourceGnss;
    p.latitude_e7 = location.latitude_e7;
    p.longitude_e7 = location.longitude_e7;
    p.hdop_x100 = location.hdop_x100;
    p.satellites = location.satellites;
    if (location.altitude_valid) {
      flags |= tlp::kPeriodicAltitudeValid;
      p.altitude_mm = location.altitude_mm;
    }
    if (location.age_valid) {
      flags |= tlp::kPeriodicLocationAgeValid;
      p.location_age_seconds =
          location.age_seconds >= tlp::kLocationAgeSaturated
              ? tlp::kLocationAgeSaturated
              : static_cast<uint16_t>(location.age_seconds);
    }
  }

  // Activity counts only for this exact period; otherwise it is unknown.
  const ActivityPeriodEvidence& activity = in.activity;
  if (activity.active_inactive_classification_valid &&
      activity.period_duration_seconds == in.period_duration_seconds &&
      activity.coverage_seconds != 0 &&
      activity.coverage_seconds <= in.period_duration_seconds &&
      activity.active_seconds <= activity.coverage_seconds) {
    flags |= tlp::kPeriodicActivityValid;
    p.activity_coverage_seconds = activity.coverage_seconds;
    p.active_seconds = activity.active_seconds;
    p.activity_transition_count = activity.transition_count;
    p.movement_mean_abs_delta_mg = activity.movement_mean_abs_delta_mg;
    p.activity_quality = activity.coverage_seconds == in.period_duration_seconds
                             ? tlp::kActivityQualityComplete
                             : tlp::kActivityQualityPartial;
  }

  if (in.battery_valid) {
    flags |= tlp::kPeriodicBatteryValid;
    p.battery_mv = in.battery_mv;
    p.battery_state = in.battery_state;
    p.battery_quality = tlp::kBatteryQualityMeasured;
  }

  if (in.health.valid) {
    flags |= tlp::kPeriodicHealthValid;
    p.health_flags = in.health.flags;
    p.gnss_failure_count = saturate8(in.health.gnss_failures);
    p.sensor_failure_count = saturate8(in.health.sensor_failures);
    p.storage_failure_count = saturate8(in.health.storage_failures);
    p.radio_failure_count = saturate8(in.health.radio_failures);
  }

  p.record_flags = flags;
  const tlp::ProductSecureCodecResult verdict =
      tlp::validatePeriodicPlaintext(p);
  if (verdict == tlp::ProductSecureCodecResult::kOk) *output = p;
  return verdict;
}

}  // namespace orun_tlp
