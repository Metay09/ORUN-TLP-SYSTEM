#include "../m3/gnss_test_support.h"

namespace {

void acquireInitialFix(GnssManager& manager) {
  emitPvt(manager, pvt(1000));
  emitDop(manager, 1000);
  emitPvt(manager, pvt(2000));
  emitDop(manager, 2000);
  assert(manager.state() == State::kFixAvailable);
}

void acquireAdditionalFix(GnssManager& manager, uint32_t tow) {
  emitPvt(manager, pvt(tow));
  emitDop(manager, tow);
  assert(manager.state() == State::kFixAvailable);
}

void pendingFreshFixHoldsPowerThroughStorageBackpressure() {
  GnssManager manager;
  boot(manager);
  acquireInitialFix(manager);
  const uint32_t attempts = manager.diagnostics().acquisition_attempts;

  assert(manager.hasFreshFixForTransmission());
  assert(manager.state() == State::kFixAvailable);
  assert(sensor_power == HIGH);

  // Model one or more composition-loop passes spent reserving HistoryStore
  // sequence space before PositionFlow can consume the fix.
  manager.poll();
  manager.poll();
  assert(manager.hasFreshFixForTransmission());
  assert(manager.state() == State::kFixAvailable);
  assert(sensor_power == HIGH);
  assert(manager.diagnostics().acquisition_attempts == attempts);

  GnssFix fix{};
  assert(manager.takeFreshFixForTransmission(&fix));
  assert(!manager.hasFreshFixForTransmission());

  // Geofence continuation must still be able to reuse this exact receiver
  // session after the delayed application consumption.
  assert(manager.continueCurrentAcquisitionForAdditionalFix());
  assert(manager.state() == State::kAcquiring);
  assert(manager.additionalFixAcquisitionActive());
  assert(sensor_power == HIGH);
  assert(manager.diagnostics().acquisition_attempts == attempts);
}

void unconsumedFreshFixExpiresAndStillAllowsLowPower() {
  GnssManager manager;
  boot(manager);
  acquireInitialFix(manager);
  const uint32_t expired_before =
      manager.diagnostics().expired_unsent_fixes;

  // Deferring low power must stay bounded by the existing freshness contract;
  // a permanently blocked consumer must not keep GNSS powered indefinitely.
  test_now += gnss_config::kFreshFixMaxAgeMs;
  manager.poll();

  assert(!manager.hasFreshFixForTransmission());
  assert(manager.diagnostics().expired_unsent_fixes == expired_before + 1);
  assert(manager.state() == State::kSleeping);
  assert(sensor_power == LOW);
}

void twoAdditionalFixesStayInOneAcquisition() {
  GnssManager manager;
  boot(manager);
  acquireInitialFix(manager);
  GnssFix fix{};
  assert(manager.takeFreshFixForTransmission(&fix));
  const uint32_t attempts = manager.diagnostics().acquisition_attempts;
  assert(sensor_power == HIGH);

  assert(manager.continueCurrentAcquisitionForAdditionalFix());
  assert(manager.state() == State::kAcquiring);
  assert(manager.additionalFixAcquisitionActive());
  assert(manager.diagnostics().acquisition_attempts == attempts);
  acquireAdditionalFix(manager, 3000);
  assert(!manager.additionalFixAcquisitionActive());
  assert(manager.takeFreshFixForTransmission(&fix));
  assert(manager.diagnostics().acquisition_attempts == attempts);

  assert(manager.continueCurrentAcquisitionForAdditionalFix());
  acquireAdditionalFix(manager, 4000);
  assert(manager.takeFreshFixForTransmission(&fix));
  assert(manager.diagnostics().acquisition_attempts == attempts);

  manager.poll();
  assert(manager.state() == State::kSleeping);
  assert(sensor_power == LOW);
}

void cancellationExitsWithoutInventingTimeoutOrFailure() {
  GnssManager manager;
  boot(manager);
  acquireInitialFix(manager);
  GnssFix fix{};
  assert(manager.takeFreshFixForTransmission(&fix));
  const uint32_t timeouts = manager.diagnostics().acquisition_timeouts;

  assert(manager.continueCurrentAcquisitionForAdditionalFix());
  assert(manager.cancelAdditionalFixAcquisition());
  assert(!manager.additionalFixAcquisitionActive());
  assert(manager.state() == State::kFixAvailable);
  assert(!manager.cancelAdditionalFixAcquisition());

  manager.poll();
  assert(manager.state() == State::kSleeping);
  assert(manager.diagnostics().acquisition_timeouts == timeouts);
}

void continuationNeverExtendsOriginalAcquisitionCeiling() {
  GnssManager manager;
  const uint32_t anchor = boot(manager);
  acquireInitialFix(manager);
  GnssFix fix{};
  assert(manager.takeFreshFixForTransmission(&fix));
  assert(manager.continueCurrentAcquisitionForAdditionalFix());

  test_now = anchor + gnss_config::kAcquisitionTimeoutMs;
  manager.poll();
  assert(manager.state() == State::kTimeout);
  assert(!manager.additionalFixAcquisitionActive());
  assert(manager.diagnostics().acquisition_timeouts == 1);
}

void cadenceReanchorIsExplicitAndNoCatchUpBurstOccurs() {
  GnssManager manager;
  boot(manager);
  acquireInitialFix(manager);

  assert(!manager.setTrackingIntervalMsAndReanchor(60000));
  GnssFix fix{};
  assert(manager.takeFreshFixForTransmission(&fix));
  const uint32_t transition_at = test_now;
  const uint32_t attempts = manager.diagnostics().acquisition_attempts;

  assert(manager.setTrackingIntervalMsAndReanchor(60000));
  assert(manager.trackingIntervalMs() == 60000);
  manager.poll();
  assert(manager.state() == State::kIdle);
  assert(sensor_power == HIGH);
  assert(manager.diagnostics().acquisition_attempts == attempts);

  test_now = transition_at + 59999;
  manager.poll();
  assert(manager.state() == State::kIdle);
  assert(manager.diagnostics().acquisition_attempts == attempts);

  test_now = transition_at + 60000;
  manager.poll();
  assert(manager.state() == State::kStarting);
  assert(manager.diagnostics().acquisition_attempts == attempts + 1);
}

void baseCadenceReanchorSleepsUntilNewDeadline() {
  GnssManager manager;
  boot(manager);
  acquireInitialFix(manager);
  GnssFix fix{};
  assert(manager.takeFreshFixForTransmission(&fix));
  const uint32_t transition_at = test_now;
  const uint32_t attempts = manager.diagnostics().acquisition_attempts;

  assert(manager.setTrackingIntervalMsAndReanchor(180000));
  manager.poll();
  assert(manager.state() == State::kSleeping);
  assert(sensor_power == LOW);

  test_now = transition_at + 179999;
  manager.poll();
  assert(manager.state() == State::kSleeping);
  assert(manager.diagnostics().acquisition_attempts == attempts);

  test_now = transition_at + 180000;
  manager.poll();
  assert(manager.state() == State::kStarting);
  assert(manager.diagnostics().acquisition_attempts == attempts + 1);
}

}  // namespace

int main() {
  pendingFreshFixHoldsPowerThroughStorageBackpressure();
  unconsumedFreshFixExpiresAndStillAllowsLowPower();
  twoAdditionalFixesStayInOneAcquisition();
  cancellationExitsWithoutInventingTimeoutOrFailure();
  continuationNeverExtendsOriginalAcquisitionCeiling();
  cadenceReanchorIsExplicitAndNoCatchUpBurstOccurs();
  baseCadenceReanchorSleepsUntilNewDeadline();
  puts("M6D2 GNSS continuation/re-anchor checks: PASS");
}
