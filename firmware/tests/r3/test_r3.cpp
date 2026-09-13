// White-box fault injection only for an otherwise unreachable retained-session
// candidate. All matching/UTC/state decisions execute production GnssManager.
#include <SparkFun_u-blox_GNSS_Arduino_Library.h>
#define private public
#include "gnss_manager.h"
#undef private
#include "../m3/gnss_test_support.h"
#include "node_role.h"

void ageIsNotRenewed(bool dop_first) {
  GnssManager manager;
  boot(manager, UINT32_MAX - 3000);
  emitPvt(manager, pvt(1000)); emitDop(manager, 1000);
  const uint32_t first_at = test_now;
  if (dop_first) emitDop(manager, 2000);
  else emitPvt(manager, pvt(2000));
  test_now += 4000;
  // Duplicate candidate arrival must not renew its original capture time.
  if (dop_first) emitDop(manager, 2000);
  else emitPvt(manager, pvt(2000));
  if (dop_first) emitPvt(manager, pvt(2000));
  else emitDop(manager, 2000);
  GnssFix fix{};
  assert(manager.takeFreshFixForTransmission(&fix));
  assert(fix.captured_at_ms == (dop_first ? test_now : first_at));
  assert(!manager.takeFreshFixForTransmission(&fix));
  assert(manager.diagnostics().successful_fresh_fixes == 1);

  GnssManager expiring;
  boot(expiring);
  emitPvt(expiring, pvt(1000)); emitDop(expiring, 1000);
  emitPvt(expiring, pvt(2000));
  test_now += 4999;
  emitDop(expiring, 2000);
  ++test_now;
  assert(!expiring.takeFreshFixForTransmission(&fix));
  assert(expiring.diagnostics().expired_unsent_fixes == 1);
}

void sessionBoundary(bool old_dop) {
  GnssManager manager;
  const auto anchor = boot(manager);
  emitPvt(manager, pvt(1000)); emitDop(manager, 1000);
  if (old_dop) emitDop(manager, 0);
  else emitPvt(manager, pvt(0));
  const auto generation_a = manager.session_generation_;
  test_now = anchor + gnss_config::kAcquisitionTimeoutMs;
  manager.poll();
  auto delayed = pvt(0);
  Fake::pvt(&delayed); // Callback after timeout is ineligible.
  manager.poll();
  assert(manager.state() == State::kSleeping);
  test_now = anchor + gnss_config::kTrackingIntervalMs;
  manager.poll();
  test_now += gnss_config::kPowerSettleMs;
  // Model library pending copy left by configuration; STARTING drains it.
  Fake::parsePvt(delayed);
  prepare(manager);
  assert(manager.session_generation_ != generation_a);
  assert(!manager.has_candidate_fix_ && !manager.has_latest_hdop_);
  emitPvt(manager, pvt(604799000)); emitDop(manager, 604799000); // Week rollover.
  if (old_dop) emitPvt(manager, pvt(0));
  else emitDop(manager, 0);
  GnssFix fix{};
  assert(!manager.takeFreshFixForTransmission(&fix));

  // Inject a candidate retained from A with the SAME iTOW and a young local
  // age: generation alone must reject it, independently of clear/age guards.
  if (old_dop) {
    manager.has_latest_hdop_ = true;
    manager.latest_hdop_itow_ = 0;
    manager.dop_received_at_ms_ = test_now;
    manager.dop_generation_ = generation_a;
  } else {
    manager.has_candidate_fix_ = true;
    manager.candidate_fix_itow_ = 0;
    manager.candidate_fix_.captured_at_ms = test_now;
    manager.pvt_generation_ = generation_a;
  }
  manager.considerPositionFix();
  assert(!manager.takeFreshFixForTransmission(&fix));
  manager.clearCandidates();
  emitDop(manager, 0);
  emitPvt(manager, pvt(0));
  assert(manager.takeFreshFixForTransmission(&fix)); // B's own repeated iTOW is legal.
}

void utcSnapshotAndWire() {
  GnssManager manager;
  boot(manager);
  emitPvt(manager, pvt(1000)); emitDop(manager, 1000);
  auto a = pvt(2000);
  a.valid.bits.validDate = a.valid.bits.validTime = true;
  emitPvt(manager, a);
  auto b = a;
  b.iTOW = 3000; b.lat += 100; b.sec += 10;
  // Mutate the library's current cache only after A's callback. The accepted
  // candidate must keep A's UTC snapshot and coordinates.
  Fake::current_pvt = b;
  Fake cache;
  assert(cache.getUnixEpoch(0) == 1700000010);
  emitDop(manager, 2000);
  GnssFix fix{};
  assert(manager.takeFreshFixForTransmission(&fix));
  assert(fix.latitude_e7 == a.lat && fix.utc_epoch_seconds == 1700000000);
  assert(fix.flags & tlp::kPositionFlagValidUtcTime);
  const tlp::PositionPacket packet{1, 2, fix.utc_epoch_seconds, fix.latitude_e7,
      fix.longitude_e7, fix.altitude_mm, fix.hdop_x100, fix.satellites, fix.flags};
  uint8_t bytes[tlp::kPositionPacketSize];
  assert(tlp::serializePositionPacket(packet, bytes, sizeof(bytes)));
  tlp::PositionPacket decoded{};
  assert(tlp::deserializePositionPacket(bytes, sizeof(bytes), &decoded));
  assert(sizeof(bytes) == 34 && bytes[0] == 1);
  assert(decoded.gnss_utc_epoch_seconds == 1700000000 && decoded.latitude_e7 == a.lat);
}

void utcValidity() {
  struct Case { UtcSnapshot utc; bool valid; uint32_t epoch; };
  const Case cases[] = {
    {{1970,1,1,0,0,0}, true, 0}, {{1969,12,31,23,59,59}, false, 0},
    {{2000,2,29,0,0,0}, true, 951782400}, {{2024,2,29,0,0,0}, true, 1709164800},
    {{2023,2,29,0,0,0}, false, 0}, {{2100,2,29,0,0,0}, false, 0},
    {{2100,3,1,0,0,0}, true, 4107542400},
    {{2106,2,7,6,28,15}, true, UINT32_MAX}, {{2106,2,7,6,28,16}, false, 0},
    {{2107,1,1,0,0,0}, false, 0}, {{0,1,1,0,0,0}, false, 0},
    {{65535,1,1,0,0,0}, false, 0},
    {{2024,0,1,0,0,0}, false, 0}, {{2024,13,1,0,0,0}, false, 0},
    {{2024,1,0,0,0,0}, false, 0}, {{2024,4,31,0,0,0}, false, 0},
    {{2024,1,32,0,0,0}, false, 0}, {{2024,1,1,24,0,0}, false, 0},
    {{2024,1,1,0,60,0}, false, 0}, {{2024,1,1,0,0,61}, false, 0},
    {{2016,12,31,23,59,60}, true, 1483228800},
    {{2023,12,31,23,59,59}, true, 1704067199},
    {{2024,1,1,0,0,0}, true, 1704067200},
  };
  for (const auto& c : cases) {
    for (unsigned flags = 0; flags < 4; ++flags) {
      GnssManager manager;
      boot(manager);
      emitPvt(manager, pvt(1)); emitDop(manager, 1);
      auto value = pvt(2);
      value.year = c.utc.year; value.month = c.utc.month; value.day = c.utc.day;
      value.hour = c.utc.hour; value.min = c.utc.minute; value.sec = c.utc.second;
      value.valid.bits.validDate = flags & 1;
      value.valid.bits.validTime = flags & 2;
      emitPvt(manager, value); emitDop(manager, 2);
      GnssFix fix{};
      assert(manager.takeFreshFixForTransmission(&fix));
      const bool valid = c.valid && flags == 3;
      assert(bool(fix.flags & tlp::kPositionFlagValidUtcTime) == valid);
      assert(fix.utc_epoch_seconds == (valid ? c.epoch : 0));
    }
  }
}

void repeatedStaleEpoch() {
  for (bool dop_first : {false, true}) {
    GnssManager manager;
    boot(manager);
    emitPvt(manager, pvt(604799000)); emitDop(manager, 604799000);
    if (dop_first) emitDop(manager, 0); else emitPvt(manager, pvt(0));
    test_now += gnss_config::kFreshFixMaxAgeMs;
    // Repeated iTOW must not replace the old candidate with an apparently young one.
    if (dop_first) { emitDop(manager, 0); emitPvt(manager, pvt(0)); }
    else { emitPvt(manager, pvt(0)); emitDop(manager, 0); }
    GnssFix fix{};
    assert(!manager.takeFreshFixForTransmission(&fix));
    emitPvt(manager, pvt(1000)); emitDop(manager, 1000);
    assert(manager.takeFreshFixForTransmission(&fix));
  }
}

void detectionRetry(bool eventually_present) {
  GnssManager manager;
  RoleController role;
  test_now = UINT32_MAX - 4000;
  Fake::pending.clear(); Fake::callback_valid = false;
  Fake::present = false; Fake::configuration_ok = true;
  const unsigned calls = Fake::detection_calls;
  manager.begin();
  test_now += 1000; manager.poll();
  test_now += 1000; manager.poll(); manager.poll();
  assert(manager.state() == State::kDetectionBackoff && !manager.detectionComplete());
  for (uint8_t attempt = 1; attempt < gnss_config::kDetectionMaxAttempts; ++attempt) {
    const uint32_t retry_at = test_now + gnss_config::kDetectionRetryBackoffMs * attempt;
    for (unsigned n = 0; n < 1000; ++n) manager.poll();
    assert(Fake::detection_calls == calls + attempt);
    test_now = retry_at - 1; manager.poll();
    assert(Fake::detection_calls == calls + attempt);
    test_now = retry_at;
    Fake::present = eventually_present;
    manager.poll(); manager.poll();
    if (eventually_present) break;
  }
  assert(manager.detectionComplete());
  role.updateAutomatic(manager.detectionComplete(), manager.detected());
  assert(role.role() == (eventually_present ? NodeRole::kTracker : NodeRole::kBase));
  if (eventually_present) {
    assert(manager.diagnostics().detection_retries == 1);
    assert(manager.diagnostics().acquisition_attempts == 1);
    prepare(manager);
  } else {
    assert(manager.state() == State::kNotPresent && sensor_power == LOW);
    assert(manager.diagnostics().detection_retries == 2);
    for (unsigned n = 0; n < 1000; ++n) { test_now += 1000; manager.poll(); }
    assert(Fake::detection_calls == calls + gnss_config::kDetectionMaxAttempts);
  }
  role.applyOverride(NodeRole::kRelay);
  role.updateAutomatic(true, true);
  assert(role.role() == NodeRole::kRelay);
}

void drainAndPartialDopBoundary() {
  GnssManager manager;
  boot(manager);
  // Exercise the production drain independently of transport's poll throttle.
  manager.state_ = State::kStarting;
  Fake::read_ok = false;
  auto old = pvt(1000);
  Fake::parsePvt(old);
  manager.poll();
  assert(manager.state() == State::kStarting && !Fake::callback_valid);
  assert(Fake::last_read_polling_wait == 0);
  assert(Fake::polling_wait == gnss_config::kI2cPollingWaitMs);
  assert(!manager.has_candidate_fix_);
  Fake::read_ok = true;
  const auto reads = Fake::reads;
  for (unsigned n = 0; n < 100; ++n) manager.poll();
  assert(Fake::reads == reads && manager.state() == State::kStarting);
  test_now += gnss_config::kI2cPollingWaitMs;
  manager.poll();
  assert(manager.state() == State::kAcquiring);
  emitPvt(manager, pvt(1000));
  emitPvt(manager, pvt(2000));
  // A partial old DOP completed after drain must not pair with this PVT.
  emitDop(manager, 2000);
  GnssFix fix{};
  assert(!manager.takeFreshFixForTransmission(&fix));
  emitPvt(manager, pvt(3000)); emitDop(manager, 3000);
  assert(manager.takeFreshFixForTransmission(&fix));
}

void receiverBacklogIsNotFresh() {
  {
    GnssManager manager;
    boot(manager);
    emitPvt(manager, pvt(1000)); emitDop(manager, 1000); // Initial boundaries.

    // Model one delayed I2C batch containing multiple PVT epochs. SparkFun
    // preserves the first callback copy (2000) while current_pvt advances to
    // the newest parsed epoch (3000). DOP is dispatched before PVT.
    test_now += 1000;
    Fake::pending.push_back([]() {
      UBX_NAV_DOP_data_t value{2000, 123};
      Fake::dop(&value);
    });
    Fake::parsePvt(pvt(2000));
    Fake::parsePvt(pvt(3000));
    manager.poll();
    GnssFix fix{};
    assert(!manager.takeFreshFixForTransmission(&fix));
    assert(manager.diagnostics().receiver_backlog_rejected == 1);
    assert(!manager.has_candidate_fix_ && !manager.has_latest_hdop_);

    emitPvt(manager, pvt(4000)); emitDop(manager, 4000);
    assert(manager.takeFreshFixForTransmission(&fix));
  }

  {
    GnssManager manager;
    boot(manager);
    emitPvt(manager, pvt(1000)); emitDop(manager, 1000); // Initial boundaries.

    // A lone buffered PVT can equal the mutable current cache, so cache
    // comparison alone cannot expose its age. A callback silence at the exact
    // freshness limit makes the first resumed epoch a resync boundary.
    test_now += gnss_config::kFreshFixMaxAgeMs;
    emitPvt(manager, pvt(2000)); emitDop(manager, 2000);
    GnssFix fix{};
    assert(!manager.takeFreshFixForTransmission(&fix));
    assert(manager.diagnostics().receiver_backlog_rejected == 1);

    emitPvt(manager, pvt(3000)); emitDop(manager, 3000);
    assert(manager.takeFreshFixForTransmission(&fix));
  }
}

int main() {
  ageIsNotRenewed(false); ageIsNotRenewed(true);
  sessionBoundary(false); sessionBoundary(true);
  utcSnapshotAndWire(); utcValidity(); repeatedStaleEpoch();
  detectionRetry(true); detectionRetry(false);
  drainAndPartialDopBoundary(); receiverBacklogIsNotFresh();
  puts("R3 capture/session, UTC snapshot/wire, backlog and detection checks: PASS");
}
