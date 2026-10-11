// SF5 PERIODIC record builder (product_period_builder.h): runtime facts of one
// report period -> canonical SF5B schema-v1 plaintext.
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "product_period_builder.h"
#include "sf5b_product_vectors.h"
#include "tlp_v2_product_secure.h"

using namespace orun_tlp;
using R = tlp::ProductSecureCodecResult;
namespace v = orun_tlp::sf5b_vectors;

namespace {

// The same facts as the pinned PERIODIC vector.
PeriodicRecordInputs goldenInputs() {
  PeriodicRecordInputs in;
  in.record_incarnation = 0xA1A2A3A4A5A6A7A8ULL;
  in.record_sequence = 257;
  in.period_duration_seconds = 3600;
  in.period_end_time_valid = true;
  in.period_end_epoch_seconds = 1791730800UL;
  in.location.valid = true;
  in.location.latitude_e7 = 376102718;
  in.location.longitude_e7 = 280545028;
  in.location.altitude_mm = 125000;
  in.location.altitude_valid = true;
  in.location.hdop_x100 = 186;
  in.location.satellites = 5;
  in.location.age_valid = true;
  in.location.age_seconds = 12;
  in.activity.period_duration_seconds = 3600;
  in.activity.coverage_seconds = 3300;
  in.activity.unknown_seconds = 300;
  in.activity.active_seconds = 900;
  in.activity.transition_count = 14;
  in.activity.movement_mean_abs_delta_mg = 37;
  in.activity.movement_evidence_present = true;
  in.activity.active_inactive_classification_valid = true;
  in.battery_valid = true;
  in.battery_mv = 4012;
  in.battery_state = 1;
  in.health.valid = true;
  in.health.flags = tlp::kHealthGnssDegraded;
  in.health.gnss_failures = 1;
  return in;
}

void encodeEq(const tlp::PeriodicObservationPlaintext& p, const char* hex) {
  uint8_t out[tlp::kPeriodicPlaintextSize];
  uint8_t expected[tlp::kPeriodicPlaintextSize];
  assert(tlp::encodePeriodicPlaintext(p, out, sizeof(out)) == R::kOk);
  assert(v::fromHex(hex, expected, sizeof(expected)));
  assert(memcmp(out, expected, sizeof(out)) == 0);
}

void golden() {
  tlp::PeriodicObservationPlaintext p;
  assert(buildPeriodicPlaintext(goldenInputs(), &p) == R::kOk);
  encodeEq(p, v::kPeriodicPlaintextHex);
}

void canonicalWhenAbsent() {
  // Nothing known but the period: every validity bit clear, canonical values.
  PeriodicRecordInputs in;
  in.record_incarnation = 1;
  in.record_sequence = 1;
  in.period_duration_seconds = 180;
  in.location.latitude_e7 = 123;  // stale value behind a clear bit
  in.location.hdop_x100 = 99;
  in.battery_mv = 4100;
  in.health.gnss_failures = 7;
  tlp::PeriodicObservationPlaintext p;
  assert(buildPeriodicPlaintext(in, &p) == R::kOk);
  assert(p.record_flags == 0);
  assert(p.location_source == tlp::kLocationSourceUnknown);
  assert(p.latitude_e7 == 0 && p.hdop_x100 == 0);
  assert(p.location_age_seconds == tlp::kLocationAgeUnknown);
  assert(p.battery_mv == 0 && p.gnss_failure_count == 0);
  assert(p.activity_quality == tlp::kActivityQualityUnknown);

  // A fix without trusted age or altitude keeps those bits clear.
  in.location = goldenInputs().location;
  in.location.altitude_valid = false;
  in.location.age_valid = false;
  assert(buildPeriodicPlaintext(in, &p) == R::kOk);
  assert(p.record_flags == tlp::kPeriodicLocationValid);
  assert(p.altitude_mm == 0);
  assert(p.location_age_seconds == tlp::kLocationAgeUnknown);

  // UTC flag without a value is not a valid time.
  in.period_end_time_valid = true;
  in.period_end_epoch_seconds = 0;
  assert(buildPeriodicPlaintext(in, &p) == R::kOk);
  assert((p.record_flags & tlp::kPeriodicPeriodTimeValid) == 0);
}

void saturation() {
  PeriodicRecordInputs in = goldenInputs();
  in.location.age_seconds = 70000;
  in.health.gnss_failures = 1000;
  in.health.radio_failures = 255;
  in.health.storage_failures = 254;
  tlp::PeriodicObservationPlaintext p;
  assert(buildPeriodicPlaintext(in, &p) == R::kOk);
  assert(p.location_age_seconds == tlp::kLocationAgeSaturated);
  assert(p.gnss_failure_count == 255 && p.radio_failure_count == 255 &&
         p.storage_failure_count == 254);
  in.location.age_seconds = tlp::kLocationAgeSaturated - 1U;
  assert(buildPeriodicPlaintext(in, &p) == R::kOk);
  assert(p.location_age_seconds == tlp::kLocationAgeSaturated - 1U);
}

void activity() {
  tlp::PeriodicObservationPlaintext p;
  // Evidence of another period, or unclassified, is not this period's.
  PeriodicRecordInputs in = goldenInputs();
  in.activity.period_duration_seconds = 180;
  assert(buildPeriodicPlaintext(in, &p) == R::kOk);
  assert((p.record_flags & tlp::kPeriodicActivityValid) == 0);
  assert(p.activity_coverage_seconds == 0 &&
         p.movement_mean_abs_delta_mg == 0);
  in = goldenInputs();
  in.activity.active_inactive_classification_valid = false;
  assert(buildPeriodicPlaintext(in, &p) == R::kOk);
  assert((p.record_flags & tlp::kPeriodicActivityValid) == 0);
  in = goldenInputs();
  in.activity.coverage_seconds = 0;
  in.activity.active_seconds = 0;
  assert(buildPeriodicPlaintext(in, &p) == R::kOk);
  assert(p.activity_quality == tlp::kActivityQualityUnknown);

  // Full coverage is COMPLETE; a 3-minute period with one 4 s window PARTIAL.
  in = goldenInputs();
  in.activity.coverage_seconds = 3600;
  assert(buildPeriodicPlaintext(in, &p) == R::kOk);
  assert(p.activity_quality == tlp::kActivityQualityComplete);
  in = goldenInputs();
  in.period_duration_seconds = 180;
  in.activity.period_duration_seconds = 180;
  in.activity.coverage_seconds = 4;
  in.activity.active_seconds = 4;
  in.activity.transition_count = 0;
  assert(buildPeriodicPlaintext(in, &p) == R::kOk);
  assert(p.activity_quality == tlp::kActivityQualityPartial &&
         p.active_seconds == 4 && p.activity_coverage_seconds == 4);

  // Inconsistent evidence is dropped, never sent.
  in = goldenInputs();
  in.activity.active_seconds = 3301;
  assert(buildPeriodicPlaintext(in, &p) == R::kOk);
  assert((p.record_flags & tlp::kPeriodicActivityValid) == 0);
}

void refusals() {
  tlp::PeriodicObservationPlaintext p;
  p.record_sequence = 0x5A5A;
  PeriodicRecordInputs in = goldenInputs();
  in.record_sequence = 0;
  assert(buildPeriodicPlaintext(in, &p) == R::kBadIdentity);
  in = goldenInputs();
  in.period_duration_seconds = 0;
  assert(buildPeriodicPlaintext(in, &p) == R::kBadTime);
  in = goldenInputs();
  in.location.latitude_e7 = 900000001;
  assert(buildPeriodicPlaintext(in, &p) == R::kBadLocation);
  in = goldenInputs();
  in.battery_state = 4;
  assert(buildPeriodicPlaintext(in, &p) == R::kBadEnum);
  in = goldenInputs();
  in.health.flags = 1U << 12;
  assert(buildPeriodicPlaintext(in, &p) == R::kBadHealth);
  assert(p.record_sequence == 0x5A5A);  // untouched on failure
  assert(buildPeriodicPlaintext(goldenInputs(), nullptr) ==
         R::kInvalidArgument);
}

}  // namespace

int main() {
  golden();
  canonicalWhenAbsent();
  saturation();
  activity();
  refusals();
  puts("SF5 PERIODIC record builder checks: PASS");
  return 0;
}
