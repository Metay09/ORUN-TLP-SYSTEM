#include "gnss_manager.h"

#include <Arduino.h>
#include <Wire.h>

#include "gnss_config.h"
#include "gnss_utc.h"
#include "monotonic_time.h"
#include "tlp_position_packet.h"

namespace orun_tlp {
namespace {

GnssManager* active_gnss_manager = nullptr;
SFE_UBLOX_GNSS gnss;

constexpr int32_t kMinLatitudeE7 = -900000000;
constexpr int32_t kMaxLatitudeE7 = 900000000;
constexpr int32_t kMinLongitudeE7 = -1800000000;
constexpr int32_t kMaxLongitudeE7 = 1800000000;

bool isNavigationFix(uint8_t fix_type) {
  return fix_type == 2 || fix_type == 3 || fix_type == 4;
}

bool is3dFix(uint8_t fix_type) { return fix_type == 3 || fix_type == 4; }

bool coordinatesAreInRange(int32_t latitude_e7, int32_t longitude_e7) {
  return latitude_e7 >= kMinLatitudeE7 && latitude_e7 <= kMaxLatitudeE7 &&
         longitude_e7 >= kMinLongitudeE7 && longitude_e7 <= kMaxLongitudeE7;
}

}  // namespace

void GnssManager::begin() {
  *this = GnssManager{};
  active_gnss_manager = this;
  pinMode(WB_IO2, OUTPUT);
  digitalWrite(WB_IO2, LOW);
  state_ = State::kPowerOff;
  state_changed_at_ms_ = monotonic::nowMs();
}

void GnssManager::poll() {
  const uint32_t now = monotonic::nowMs();
  expireFreshFix(now);
  if ((state_ == State::kStarting || state_ == State::kAcquiring) &&
      monotonic::elapsed(now, acquisition_started_at_ms_,
                         gnss_config::kAcquisitionTimeoutMs)) {
    clearCandidates();
    fresh_fix_ready_ = false;
    ++diagnostics_.acquisition_timeouts;
    needs_configuration_ = true;  // Reapply volatile config next time (e.g. reset).
    state_ = State::kTimeout;
    Serial.println(F("GNSS ACQUIRE timeout"));
    return;
  }
  switch (state_) {
    case State::kPowerOff:
      if (monotonic::elapsed(now, state_changed_at_ms_, gnss_config::kPowerSettleMs)) {
        digitalWrite(WB_IO2, HIGH);
        state_ = State::kPowerOnWait;
        state_changed_at_ms_ = now;
      }
      return;

    case State::kPowerOnWait:
      if (monotonic::elapsed(now, state_changed_at_ms_, gnss_config::kPowerSettleMs)) {
        state_ = State::kDetecting;
      }
      return;

    case State::kDetecting: {
      if (detection_attempts_++ != 0) ++diagnostics_.detection_retries;
      Wire.begin();
      if (!gnss.begin(Wire, gnss_config::kI2cAddress,
                      gnss_config::kConfigurationMaxWaitMs)) {
        Serial.println(F("GNSS: not detected"));
        state_changed_at_ms_ = monotonic::nowMs();
        state_ = detection_attempts_ < gnss_config::kDetectionMaxAttempts
                     ? State::kDetectionBackoff : State::kNotPresent;
        if (state_ == State::kNotPresent) digitalWrite(WB_IO2, LOW);
        return;
      }

      detected_ = true;
      Serial.println(F("GNSS: detected"));
      // First acquisition starts immediately after detection. This time anchors
      // the fixed schedule; neither TTFF nor radio TX duration shifts its phase.
      next_due_at_ms_ = monotonic::nowMs();
      startAcquisition(next_due_at_ms_);
      return;
    }

    case State::kStarting:
      prepareAcquisition();
      return;

    case State::kAcquiring:
      gnss.checkUblox();
      gnss.checkCallbacks();
      return;

    case State::kIdle:
      // Drain auto messages while tracking continuously, but do not promote
      // fixes outside an acquisition or allow the receiver FIFO to backlog.
      gnss.checkUblox();
      gnss.checkCallbacks();
      if (monotonic::reached(now, next_due_at_ms_)) {
        startAcquisition(monotonic::nowMs());
      }
      return;

    case State::kSleeping:
      // No GNSS bus traffic while the sensor supply is switched off.
      if (monotonic::reached(now, next_due_at_ms_)) {
        startAcquisition(now);
      }
      return;

    case State::kFixAvailable:
    case State::kTimeout:
    case State::kFailure:
      enterLowPower(now);
      return;

    case State::kDetectionBackoff:
      if (monotonic::elapsed(now, state_changed_at_ms_,
                            gnss_config::kDetectionRetryBackoffMs * detection_attempts_))
        state_ = State::kDetecting;
      return;

    case State::kNotPresent:
      return;
  }
}

void GnssManager::clearCandidates() {
  has_candidate_fix_ = false;
  has_latest_hdop_ = false;
}

void GnssManager::startAcquisition(uint32_t now) {
  ++session_generation_;
  if (state_ == State::kSleeping) {
    digitalWrite(WB_IO2, HIGH);
    state_changed_at_ms_ = now;
    waiting_for_power_ = true;
    needs_configuration_ = true;
  }
  clearCandidates();
  fresh_fix_ready_ = false;
  has_boundary_epoch_ = false;
  has_dop_boundary_epoch_ = false;
  has_last_pvt_callback_time_ = false;
  last_pvt_callback_at_ms_ = 0;
  waiting_for_drain_ = false;
  acquisition_started_at_ms_ = now;
  next_due_at_ms_ = monotonic::nextFuture(
      now, next_due_at_ms_, gnss_config::kTrackingIntervalMs);
  configuration_step_ = needs_configuration_ ? 0 : 5;
  state_ = State::kStarting;
  ++diagnostics_.acquisition_attempts;
  Serial.println(F("GNSS ACQUIRE start"));
}

void GnssManager::prepareAcquisition() {
  if (waiting_for_power_) {
    if (!monotonic::elapsed(monotonic::nowMs(), state_changed_at_ms_,
                            gnss_config::kPowerSettleMs)) {
      return;
    }
    waiting_for_power_ = false;
  }
  // At most one bounded library configuration operation per cooperative pass.
  // The library may do several 250 ms transactions inside one operation; no
  // operation waits for a fix. Radio IRQ/task processing remains enabled.
  bool ok = true;
  switch (configuration_step_) {
    case 0:
      ok = gnss.setI2COutput(COM_TYPE_UBX, gnss_config::kConfigurationMaxWaitMs);
      break;
    case 1:
      ok = gnss.setNavigationFrequency(gnss_config::kNavigationFrequencyHz,
                                       gnss_config::kConfigurationMaxWaitMs);
      break;
    case 2:
      ok = gnss.powerSaveMode(false, gnss_config::kConfigurationMaxWaitMs);
      break;
    case 3:
      ok = gnss.setAutoPVTcallbackPtr(onPvt, gnss_config::kConfigurationMaxWaitMs);
      break;
    case 4:
      ok = gnss.setAutoDOPcallbackPtr(onDop, gnss_config::kConfigurationMaxWaitMs);
      break;
    default:
      // Consume pending library callbacks and receiver output while acceptance
      // is gated by STARTING. Do not mutate library parser/cache internals:
      // flushPVT/flushDOP only clear getter flags, not pending callback copies.
      // checkUblox may otherwise skip the bus because of its polling gate.
      // Require a successful read of the available byte batch. False (empty
      // or transport error) keeps STARTING gated until data or the timeout.
      if (waiting_for_drain_ && !monotonic::elapsed(
              monotonic::nowMs(), drain_attempted_at_ms_,
              gnss_config::kI2cPollingWaitMs)) return;
      gnss.setI2CpollingWait(0);
      ok = gnss.checkUblox();
      gnss.setI2CpollingWait(gnss_config::kI2cPollingWaitMs);
      gnss.checkCallbacks();
      clearCandidates();
      if (!ok) {
        waiting_for_drain_ = true;
        drain_attempted_at_ms_ = monotonic::nowMs();
        return;
      }
      waiting_for_drain_ = false;
      needs_configuration_ = false;
      state_ = State::kAcquiring;
      return;
  }
  if (!ok) {
    ++diagnostics_.configuration_failures;
    needs_configuration_ = true;
    clearCandidates();
    state_ = State::kFailure;
    Serial.println(F("GNSS ACQUIRE configuration failed"));
    return;
  }
  ++configuration_step_;
}

void GnssManager::enterLowPower(uint32_t now) {
  clearCandidates();
  next_due_at_ms_ = monotonic::nextFuture(
      now, next_due_at_ms_, gnss_config::kTrackingIntervalMs);
  const uint32_t remaining_ms = next_due_at_ms_ - now;
  if (gnss_config::keepTracking(gnss_config::kTrackingIntervalMs, remaining_ms)) {
    state_ = State::kIdle;
    Serial.println(F("GNSS idle (continuous tracking)"));
    return;
  }

  // RAK12500 official I2C example uses WB_IO2 LOW/HIGH for slot power.
  // The published schematic marks RTC crystal X4 NC, so timed software backup
  // and hot-start retention are not assumed. See M3's hardware evidence.
  digitalWrite(WB_IO2, LOW);
  needs_configuration_ = true;
  state_ = State::kSleeping;
  Serial.println(F("GNSS low power (WB_IO2 off)"));
}

void GnssManager::expireFreshFix(uint32_t now) {
  if (fresh_fix_ready_ && monotonic::elapsed(
          now, fresh_fix_.captured_at_ms, gnss_config::kFreshFixMaxAgeMs)) {
    fresh_fix_ready_ = false;
    ++diagnostics_.expired_unsent_fixes;
    Serial.println(F("GNSS unsent fix expired"));
  }
}

bool GnssManager::takeFreshFixForTransmission(GnssFix* fix) {
  expireFreshFix(monotonic::nowMs());
  if (fix == nullptr || !fresh_fix_ready_) {
    return false;
  }

  *fix = fresh_fix_;
  fresh_fix_ready_ = false;
  return true;
}

bool GnssManager::detected() const { return detected_; }

void GnssManager::onPvt(UBX_NAV_PVT_data_t* pvt_data) {
  if (active_gnss_manager != nullptr && pvt_data != nullptr) {
    active_gnss_manager->handlePvt(*pvt_data);
  }
}

void GnssManager::onDop(UBX_NAV_DOP_data_t* dop_data) {
  if (active_gnss_manager != nullptr && dop_data != nullptr) {
    active_gnss_manager->handleDop(*dop_data);
  }
}

void GnssManager::handlePvt(const UBX_NAV_PVT_data_t& pvt_data) {
  if (state_ != State::kAcquiring) {
    return;
  }
  const uint32_t received_at = monotonic::nowMs();

  // SparkFun 2.2.29 keeps the first unconsumed callback copy while continuing
  // to update packetUBXNAVPVT->data for every later PVT parsed in the same I2C
  // batch. Immediately after checkUblox(), getTimeOfWeek(0) therefore exposes
  // the newest parsed PVT iTOW without a new bus wait: processUBXpacket marks
  // iTOW fresh before checkCallbacks() runs. A mismatch proves this callback is
  // receiver/FIFO backlog. A >= freshness-limit callback silence is also
  // treated as a resynchronization boundary, covering a lone stale buffered PVT
  // whose cache iTOW cannot differ. Both cases conservatively discard one epoch.
  const uint32_t newest_parsed_itow = gnss.getTimeOfWeek(0);
  const bool callback_gap =
      has_last_pvt_callback_time_ &&
      monotonic::elapsed(received_at, last_pvt_callback_at_ms_,
                         gnss_config::kFreshFixMaxAgeMs);
  last_pvt_callback_at_ms_ = received_at;
  has_last_pvt_callback_time_ = true;
  if (callback_gap || newest_parsed_itow != pvt_data.iTOW) {
    clearCandidates();
    boundary_epoch_ = pvt_data.iTOW;
    has_boundary_epoch_ = true;
    ++diagnostics_.receiver_backlog_rejected;
    return;
  }

  // After draining, the first observed epoch is a boundary, never a fix to
  // transmit. Require an epoch change, even if the first PVT was invalid.
  // This also rejects a partial pre-acquisition packet completed after wake.
  if (!has_boundary_epoch_) {
    boundary_epoch_ = pvt_data.iTOW;
    has_boundary_epoch_ = true;
  }
  if (!pvt_data.flags.bits.gnssFixOK || !isNavigationFix(pvt_data.fixType) ||
      pvt_data.flags3.bits.invalidLlh ||
      !coordinatesAreInRange(pvt_data.lat, pvt_data.lon)) {
    has_candidate_fix_ = false;
    ++diagnostics_.invalid_fixes;
    return;
  }
  if (pvt_data.iTOW == boundary_epoch_) {
    has_candidate_fix_ = false;
    return;
  }
  // Repeated delivery of a valid candidate must not renew its age.
  if (has_candidate_fix_ && candidate_fix_itow_ == pvt_data.iTOW) return;

  // iTOW is not monotonic across GPS week rollover. Once another PVT epoch is
  // observed, an equal iTOW from a future week must remain eligible.
  if (has_last_promoted_fix_itow_ &&
      pvt_data.iTOW != last_promoted_fix_itow_) {
    has_last_promoted_fix_itow_ = false;
  }

  candidate_fix_.captured_at_ms = received_at;
  pvt_generation_ = session_generation_;
  candidate_fix_.latitude_e7 = pvt_data.lat;
  candidate_fix_.longitude_e7 = pvt_data.lon;
  candidate_fix_.altitude_mm = pvt_data.height;
  candidate_fix_.satellites = pvt_data.numSV;
  candidate_fix_.flags = tlp::kPositionFlagValidFix;
  candidate_fix_.utc_epoch_seconds = 0;
  if (pvt_data.valid.bits.validDate && pvt_data.valid.bits.validTime) {
    // Convert immediately from this immutable callback snapshot, never date/time getters.
    const UtcSnapshot utc{pvt_data.year, pvt_data.month, pvt_data.day,
                          pvt_data.hour, pvt_data.min, pvt_data.sec};
    if (utcToEpoch(utc, candidate_fix_.utc_epoch_seconds))
      candidate_fix_.flags |= tlp::kPositionFlagValidUtcTime;
    else
      ++diagnostics_.invalid_utc_snapshots;
  }
  if (is3dFix(pvt_data.fixType)) {
    candidate_fix_.flags |= tlp::kPositionFlag3dFix;
  }

  candidate_fix_itow_ = pvt_data.iTOW;
  has_candidate_fix_ = true;
  considerPositionFix();
}

void GnssManager::handleDop(const UBX_NAV_DOP_data_t& dop_data) {
  if (state_ != State::kAcquiring) {
    return;
  }
  const uint32_t received_at = monotonic::nowMs();
  // A drain can end partway through either UBX message type. Reject the
  // first DOP epoch too, even if its counterpart PVT was already observed.
  if (!has_dop_boundary_epoch_) {
    dop_boundary_epoch_ = dop_data.iTOW;
    has_dop_boundary_epoch_ = true;
  }
  if (dop_data.iTOW == dop_boundary_epoch_) return;
  if (has_latest_hdop_ && latest_hdop_itow_ == dop_data.iTOW) return;
  dop_received_at_ms_ = received_at;
  dop_generation_ = session_generation_;
  latest_hdop_itow_ = dop_data.iTOW;
  latest_hdop_x100_ = dop_data.hDOP;
  has_latest_hdop_ = true;
  considerPositionFix();
}

void GnssManager::considerPositionFix() {
  const uint32_t now = monotonic::nowMs();
  if (state_ != State::kAcquiring ||
      monotonic::elapsed(now, acquisition_started_at_ms_, gnss_config::kAcquisitionTimeoutMs) ||
      !has_candidate_fix_ || !has_latest_hdop_ ||
      pvt_generation_ != session_generation_ || dop_generation_ != session_generation_ ||
      candidate_fix_itow_ != latest_hdop_itow_ || fresh_fix_ready_ ||
      (has_last_promoted_fix_itow_ &&
       candidate_fix_itow_ == last_promoted_fix_itow_)) {
    return;
  }

  const bool stale_pvt = monotonic::elapsed(
      now, candidate_fix_.captured_at_ms, gnss_config::kFreshFixMaxAgeMs);
  const bool stale_dop = monotonic::elapsed(
      now, dop_received_at_ms_, gnss_config::kFreshFixMaxAgeMs);
  if (stale_pvt || stale_dop) {
    if (stale_pvt) ++diagnostics_.stale_pvt_rejected;
    if (stale_dop) ++diagnostics_.stale_dop_rejected;
    // Retain epoch/time so a duplicate callback cannot renew this candidate.
    return;
  }

  candidate_fix_.hdop_x100 = latest_hdop_x100_;
  fresh_fix_ = candidate_fix_;
  fresh_fix_itow_ = candidate_fix_itow_;
  fresh_fix_ready_ = true;
  last_promoted_fix_itow_ = fresh_fix_itow_;
  has_last_promoted_fix_itow_ = true;
  diagnostics_.last_ttff_ms = now - acquisition_started_at_ms_;
  ++diagnostics_.successful_fresh_fixes;
  state_ = State::kFixAvailable;
  Serial.printf("GNSS FIX ttff=%lums lat=%ld lon=%ld sats=%u hdop=%u.%02u\n",
                static_cast<unsigned long>(diagnostics_.last_ttff_ms),
                static_cast<long>(fresh_fix_.latitude_e7),
                static_cast<long>(fresh_fix_.longitude_e7),
                fresh_fix_.satellites, fresh_fix_.hdop_x100 / 100,
                fresh_fix_.hdop_x100 % 100);
}

}  // namespace orun_tlp
