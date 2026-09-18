// M7P5: tracking_interval_seconds becomes the authoritative runtime GNSS
// schedule interval, against the actual production GnssManager. Reuses the
// M3 fake u-blox/Wire harness (gnss_test_support.h) exactly as tests/m3
// already does; this file only exercises the new setTrackingIntervalMs()
// seam, not GNSS acquisition correctness itself (already covered by M3/R3).
#include "../m3/gnss_test_support.h"

// Mirrors test_m3.cpp's testFixes(dop_first=true) fix-acquisition sequence:
// the first observed PVT/DOP epoch is always a rejected "boundary" (proven
// drain semantics), so a real takeable fix needs a second, later epoch.
// `tow` must be a fresh epoch not previously used as a boundary or fix in
// this acquisition session.
void acquireFix(GnssManager& manager, uint32_t boundary_tow, uint32_t tow) {
  emitPvt(manager, pvt(boundary_tow));
  emitDop(manager, boundary_tow);
  auto value = pvt(tow);
  value.valid.bits.validDate = value.valid.bits.validTime = true;
  emitDop(manager, tow);
  emitPvt(manager, value);
  assert(manager.state() == State::kFixAvailable);
}

void testDefaultIntervalUnchangedByBlankConfig() {
  GnssManager manager;
  const auto anchor = boot(manager);
  assert(manager.trackingIntervalMs() == gnss_config::kTrackingIntervalMs);
  acquireFix(manager, 1000, 2000);
  GnssFix fix{};
  assert(manager.takeFreshFixForTransmission(&fix));
  manager.poll();
  assert(manager.state() == State::kSleeping);
  // Never called setTrackingIntervalMs(): the current main default (180s)
  // must remain exactly in effect, matching "blank device behaves exactly
  // as current main".
  test_now = anchor + gnss_config::kTrackingIntervalMs - 1;
  manager.poll();
  assert(manager.state() == State::kSleeping);
  test_now = anchor + gnss_config::kTrackingIntervalMs;
  manager.poll();
  assert(manager.state() == State::kStarting);
}

void testZeroIntervalIgnored() {
  GnssManager manager;
  boot(manager);
  manager.setTrackingIntervalMs(0);
  assert(manager.trackingIntervalMs() == gnss_config::kTrackingIntervalMs);
}

void testCustomIntervalTakesEffectFromNextSchedule() {
  GnssManager manager;
  const auto anchor = boot(manager);  // first cycle already scheduled under the default
  // Kept above gnss_config::kShortIntervalThresholdSeconds (60s) so the
  // normal sleep/wake schedule applies here, not the separate short-interval
  // continuous-tracking policy (gnss_config::keepTracking) -- that policy is
  // unrelated to and unaffected by this feature.
  manager.setTrackingIntervalMs(90000);
  assert(manager.trackingIntervalMs() == 90000);
  acquireFix(manager, 1000, 2000);
  GnssFix fix{};
  assert(manager.takeFreshFixForTransmission(&fix));
  manager.poll();
  assert(manager.state() == State::kSleeping);

  // The already-scheduled first due point is unchanged -- a shorter
  // interval configured afterward must not fire the pending schedule early.
  test_now = anchor + gnss_config::kTrackingIntervalMs - 1;
  manager.poll();
  assert(manager.state() == State::kSleeping);
  test_now = anchor + gnss_config::kTrackingIntervalMs;
  manager.poll();
  assert(manager.state() == State::kStarting);
  const uint32_t second_due = test_now;
  assert(manager.diagnostics().acquisition_attempts == 2);
  test_now += gnss_config::kPowerSettleMs;
  prepare(manager);
  acquireFix(manager, 3000, 4000);
  assert(manager.takeFreshFixForTransmission(&fix));
  manager.poll();
  assert(manager.state() == State::kSleeping);

  // The NEXT due point is now governed by the custom 90s interval, far
  // sooner than another 180s default cycle would allow.
  test_now = second_due + 90000 - 1;
  manager.poll();
  assert(manager.state() == State::kSleeping);
  test_now = second_due + 90000;
  manager.poll();
  assert(manager.state() == State::kStarting);
  assert(manager.diagnostics().acquisition_attempts == 3);
}

void testIntervalChangeDuringAcquisitionDoesNotOverlap() {
  GnssManager manager;
  boot(manager);  // leaves the manager mid-kAcquiring
  assert(manager.state() == State::kAcquiring);
  const auto attempts_before = manager.diagnostics().acquisition_attempts;
  // Changing the interval while an acquisition is in flight must not touch
  // the in-progress acquisition or trigger a second, overlapping one --
  // the interval only governs where the NEXT cycle's due point lands.
  manager.setTrackingIntervalMs(45000);
  manager.poll();
  assert(manager.state() == State::kAcquiring);
  assert(manager.diagnostics().acquisition_attempts == attempts_before);
  acquireFix(manager, 1000, 2000);
  assert(manager.diagnostics().acquisition_attempts == attempts_before);  // still just the one
}

int main() {
  testDefaultIntervalUnchangedByBlankConfig();
  testZeroIntervalIgnored();
  testCustomIntervalTakesEffectFromNextSchedule();
  testIntervalChangeDuringAcquisitionDoesNotOverlap();
  puts("M7P5 GnssManager runtime tracking interval checks: PASS");
}
