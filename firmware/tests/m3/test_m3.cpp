#include "gnss_test_support.h"

void testFixes(bool dop_first) {
  GnssManager manager;
  const auto anchor = boot(manager);
  GnssFix fix{};
  emitPvt(manager, pvt(1000));  // Boundary must never be sent.
  emitDop(manager, 1000);
  assert(!manager.takeFreshFixForTransmission(&fix));
  test_now += 1500;
  auto value = pvt(2000);
  value.valid.bits.validDate = value.valid.bits.validTime = dop_first;
  if (dop_first) emitDop(manager, 2000);
  emitPvt(manager, value);
  if (!dop_first) {
    emitDop(manager, 1999);  // Mismatched epoch is not eligible.
    assert(!manager.takeFreshFixForTransmission(&fix));
    emitDop(manager, 2000);
  }
  assert(manager.state() == State::kFixAvailable);
  assert(manager.diagnostics().last_ttff_ms == test_now - anchor);
  auto later = pvt(9000);
  later.lat = 420000000;
  Fake::pvt(&later);  // A later callback cannot overwrite the pending fix.
  UBX_NAV_DOP_data_t later_dop{9000, 999};
  Fake::dop(&later_dop);
  assert(manager.takeFreshFixForTransmission(&fix));
  assert(!manager.takeFreshFixForTransmission(&fix));
  assert(fix.latitude_e7 == value.lat && fix.hdop_x100 == 123);
  assert(fix.utc_epoch_seconds == (dop_first ? 1700000000U : 0U));
  assert(bool(fix.flags & tlp::kPositionFlagValidUtcTime) == dop_first);
  manager.poll();
  assert(manager.state() == State::kSleeping && sensor_power == LOW);
  const auto reads = Fake::reads;
  test_now += 1000;
  manager.poll();
  assert(Fake::reads == reads);  // Sleeping never polls the device.

  // Wake at the fixed phase, not fix/TX time + interval; no blocking settle.
  test_now = anchor + gnss_config::kTrackingIntervalMs;
  manager.poll();
  assert(manager.state() == State::kStarting && sensor_power == HIGH);
  auto config_calls = Fake::config_calls;
  manager.poll();
  assert(Fake::config_calls == config_calls);
  test_now += gnss_config::kPowerSettleMs;
  // Queue a stale callback during setup: STARTING must consume and discard it.
  Fake::pending.push_back([]() { auto old = pvt(2000); Fake::pvt(&old); });
  prepare(manager);
  emitDop(manager, 2000);
  assert(!manager.takeFreshFixForTransmission(&fix));
  emitPvt(manager, pvt(2000)); // Duplicate from previous cycle is a boundary.
  emitDop(manager, 2000);
  assert(!manager.takeFreshFixForTransmission(&fix));
  emitPvt(manager, pvt(3000));
  emitDop(manager, 3000);
  assert(manager.takeFreshFixForTransmission(&fix));
  assert(manager.diagnostics().successful_fresh_fixes == 2);
}

void testTimeoutAndRecovery() {
  GnssManager manager;
  const auto anchor = boot(manager, UINT32_MAX - 3000);
  emitPvt(manager, pvt(604799000)); // GPS week boundary is equality, not ordering.
  auto invalid = pvt(0);
  invalid.flags.bits.gnssFixOK = false;
  emitPvt(manager, invalid);
  invalid = pvt(1000); invalid.flags3.bits.invalidLlh = true;
  emitPvt(manager, invalid);
  invalid = pvt(2000); invalid.lat = 900000001;
  emitPvt(manager, invalid);
  invalid = pvt(3000); invalid.fixType = 1;
  emitPvt(manager, invalid);
  assert(manager.diagnostics().invalid_fixes == 4);
  emitPvt(manager, pvt(3500));  // Valid candidate, but DOP never arrives.
  test_now = anchor + gnss_config::kAcquisitionTimeoutMs;
  emitPvt(manager, pvt(4000)); // Fix exactly at timeout must be rejected.
  assert(manager.state() == State::kTimeout);
  manager.poll();
  GnssFix fix{};
  assert(!manager.takeFreshFixForTransmission(&fix));
  for (unsigned i = 0; i < 20; ++i) manager.poll();
  assert(manager.diagnostics().acquisition_attempts == 1);
  assert(manager.diagnostics().acquisition_timeouts == 1);
  test_now = anchor + gnss_config::kTrackingIntervalMs;
  manager.poll();
  test_now += gnss_config::kPowerSettleMs;
  prepare(manager);
  emitDop(manager, 3500);  // Must not pair with the timed-out candidate.
  assert(!manager.takeFreshFixForTransmission(&fix));
  emitPvt(manager, pvt(604799000)); emitDop(manager, 604799000);
  emitDop(manager, 0);
  emitPvt(manager, pvt(0));
  assert(manager.takeFreshFixForTransmission(&fix));
}

void testMissedSlotsAndFailures() {
  GnssManager manager;
  const auto anchor = boot(manager);
  // Simulate a delayed loop: one timeout, then skip every elapsed due point.
  test_now = anchor + 3 * gnss_config::kTrackingIntervalMs;
  manager.poll();
  manager.poll();
  for (unsigned i = 0; i < 20; ++i) manager.poll();
  assert(manager.diagnostics().acquisition_attempts == 1);
  test_now = anchor + 4 * gnss_config::kTrackingIntervalMs;
  manager.poll();
  test_now += gnss_config::kPowerSettleMs;
  Fake::configuration_ok = false;
  manager.poll();
  assert(manager.state() == State::kFailure);
  assert(manager.diagnostics().configuration_failures == 1);
  const uint32_t failed_at = test_now;
  manager.poll();
  assert(manager.state() == State::kSleeping);
  // Timeout then configuration failure: two in a row, so the next attempt
  // waits 2x the interval from the failure instead of the next grid point.
  assert(manager.consecutiveFailedAcquisitions() == 2);
  assert(manager.diagnostics().failure_backoffs == 1);
  Fake::configuration_ok = true;
  test_now = anchor + 5 * gnss_config::kTrackingIntervalMs;
  manager.poll();
  assert(manager.state() == State::kSleeping);
  test_now = failed_at + 2 * gnss_config::kTrackingIntervalMs - 1;
  manager.poll();
  assert(manager.state() == State::kSleeping);
  test_now = failed_at + 2 * gnss_config::kTrackingIntervalMs;
  manager.poll();
  test_now += gnss_config::kPowerSettleMs;
  prepare(manager);
  emitPvt(manager, pvt(1)); emitDop(manager, 1); emitPvt(manager, pvt(2)); emitDop(manager, 2);
  test_now += gnss_config::kFreshFixMaxAgeMs;
  GnssFix fix{};
  assert(!manager.takeFreshFixForTransmission(&fix));
  assert(manager.diagnostics().expired_unsent_fixes == 1);

  GnssManager absent;
  absent.begin();
  test_now += 1000; absent.poll();
  test_now += 1000; absent.poll();
  Fake::present = false;
  absent.poll();
  assert(!absent.detected() && absent.state() == State::kDetectionBackoff);
}

void testTimeAndPolicy() {
  monotonic::TickMillis clock;
  auto before = clock.update(UINT32_MAX - 1023, 1024);
  auto after = clock.update(0, 1024);
  assert(uint32_t(after - before) == 1000); // Core tick wrap, ~48.5 days.
  // Next advance crosses the separate 2^32 millisecond wrap (~49.7 days).
  before = clock.update(103079215, 1024);
  after = clock.update(103080239, 1024);
  assert(after < before && uint32_t(after - before) == 1000);
  const uint32_t intervals[] = {10000, 20000, 30000, 60000, 300000, 900000};
  for (auto interval : intervals) {
    for (uint32_t due : {0U, UINT32_MAX - 4999}) {
      auto next = monotonic::nextFuture(due + 120000, due, interval);
      assert(!monotonic::reached(due + 120000, next));
      assert(uint32_t(next - due) % interval == 0);
    }
    assert(gnss_config::keepTracking(interval, interval) == (interval <= 60000));
  }
  assert(gnss_config::keepTracking(900000, 1000));
}

// No sky (barn, shed): consecutive timeouts stretch the wait 2x, 4x, ... up
// to 30 minutes, with the receiver powered down; the first fix ends it.
void testFailureBackoff() {
  using gnss_config::failureBackoffMs;
  constexpr uint32_t kInterval = gnss_config::kTrackingIntervalMs;  // 180 s
  assert(failureBackoffMs(kInterval, 0) == 0);
  assert(failureBackoffMs(kInterval, 1) == 0);
  assert(failureBackoffMs(kInterval, 2) == 2 * kInterval);
  assert(failureBackoffMs(kInterval, 3) == 4 * kInterval);
  assert(failureBackoffMs(kInterval, 4) == 8 * kInterval);
  assert(failureBackoffMs(kInterval, 5) == gnss_config::kFailureBackoffMaxMs);
  assert(failureBackoffMs(kInterval, 255) == gnss_config::kFailureBackoffMaxMs);
  assert(failureBackoffMs(60000, 2) == 120000);
  assert(failureBackoffMs(29UL * 60000, 2) == gnss_config::kFailureBackoffMaxMs);
  assert(failureBackoffMs(30UL * 60000, 9) == 0);  // never stretched
  assert(failureBackoffMs(0, 9) == 0);

  GnssManager manager;
  const uint32_t anchor = boot(manager);
  uint32_t started = anchor;
  auto timeOut = [&] {
    test_now = started + gnss_config::kAcquisitionTimeoutMs;
    manager.poll();
    assert(manager.state() == State::kTimeout);
    manager.poll();
    assert(manager.state() == State::kSleeping && sensor_power == LOW);
    return test_now;
  };
  auto startsAt = [&](uint32_t due) {
    test_now = due - 1;
    manager.poll();
    assert(manager.state() == State::kSleeping);
    test_now = due;
    manager.poll();
    assert(manager.state() == State::kStarting);
    started = due;
    test_now += gnss_config::kPowerSettleMs;
    prepare(manager);
  };

  // First failure: still the normal grid.
  timeOut();
  assert(manager.consecutiveFailedAcquisitions() == 1);
  assert(manager.diagnostics().failure_backoffs == 0);
  startsAt(anchor + kInterval);
  // Then 2x, 4x, 8x, capped at 30 minutes, from the end of each failure.
  const uint32_t waits[] = {2 * kInterval, 4 * kInterval, 8 * kInterval,
                            gnss_config::kFailureBackoffMaxMs,
                            gnss_config::kFailureBackoffMaxMs};
  for (const uint32_t wait : waits) {
    const uint32_t failed_at = timeOut();
    startsAt(failed_at + wait);
  }
  assert(manager.consecutiveFailedAcquisitions() == 6);
  assert(manager.diagnostics().failure_backoffs == 5);
  assert(manager.diagnostics().acquisition_timeouts == 6);

  // Sky again: the first fresh fix ends the backoff and the grid resumes.
  emitPvt(manager, pvt(10000));  // first epoch of a session is a boundary
  emitDop(manager, 10000);
  test_now += 1000;
  emitPvt(manager, pvt(11000));
  emitDop(manager, 11000);
  GnssFix fix{};
  assert(manager.takeFreshFixForTransmission(&fix));
  assert(manager.consecutiveFailedAcquisitions() == 0);
  manager.poll();
  assert(manager.state() == State::kSleeping);
  startsAt(started + kInterval);

  // A short interval is stretched too, and the receiver sleeps through it.
  manager.setTrackingIntervalMs(60000);
  timeOut();
  // Next grid point (the old 180 s due point still stands).
  for (unsigned i = 0; i < 400 && manager.state() != State::kStarting; ++i) {
    test_now += 1000;
    manager.poll();
  }
  assert(manager.state() == State::kStarting);
  started = test_now;
  test_now += gnss_config::kPowerSettleMs;
  prepare(manager);
  const uint32_t failed_at = timeOut();
  assert(sensor_power == LOW);
  startsAt(failed_at + 120000);
}

int main() {
  testTimeAndPolicy();
  testFixes(false);
  testFixes(true);
  testTimeoutAndRecovery();
  testMissedSlotsAndFailures();
  testFailureBackoff();
  puts("M3 deterministic checks: PASS");
}
