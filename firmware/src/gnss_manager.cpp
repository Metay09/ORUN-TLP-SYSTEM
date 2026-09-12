#include "gnss_manager.h"

#include <Arduino.h>
#include <Wire.h>

#include "gnss_config.h"
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
  active_gnss_manager = this;
  pinMode(WB_IO2, OUTPUT);
  digitalWrite(WB_IO2, LOW);
  state_ = State::kPowerOff;
  state_changed_at_ms_ = millis();
}

void GnssManager::poll() {
  const uint32_t now = millis();
  switch (state_) {
    case State::kPowerOff:
      if (now - state_changed_at_ms_ >= gnss_config::kPowerSettleMs) {
        digitalWrite(WB_IO2, HIGH);
        state_ = State::kPowerOnWait;
        state_changed_at_ms_ = now;
      }
      return;

    case State::kPowerOnWait:
      if (now - state_changed_at_ms_ >= gnss_config::kPowerSettleMs) {
        state_ = State::kDetecting;
      }
      return;

    case State::kDetecting: {
      Wire.begin();
      if (!gnss.begin(Wire, gnss_config::kI2cAddress,
                      gnss_config::kConfigurationMaxWaitMs)) {
        Serial.println(F("GNSS: not detected"));
        state_ = State::kUnavailable;
        return;
      }

      gnss.setI2COutput(COM_TYPE_UBX, gnss_config::kConfigurationMaxWaitMs);
      const bool pvt_enabled = gnss.setAutoPVTcallbackPtr(
          onPvt, gnss_config::kConfigurationMaxWaitMs);
      const bool dop_enabled = gnss.setAutoDOPcallbackPtr(
          onDop, gnss_config::kConfigurationMaxWaitMs);
      if (!pvt_enabled || !dop_enabled) {
        Serial.println(F("GNSS: detected, UBX setup failed"));
        state_ = State::kUnavailable;
        return;
      }

      Serial.println(F("GNSS: detected"));
      state_ = State::kReady;
      return;
    }

    case State::kReady:
      gnss.checkUblox();
      gnss.checkCallbacks();
      return;

    case State::kUnavailable:
      return;
  }
}

bool GnssManager::takeFreshFixForTransmission(GnssFix* fix) {
  if (fix == nullptr || !fresh_fix_ready_) {
    return false;
  }

  *fix = fresh_fix_;
  fresh_fix_ready_ = false;
  last_position_tx_at_ms_ = millis();
  has_sent_position_ = true;
  return true;
}

bool GnssManager::detected() const { return state_ == State::kReady; }

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
  has_candidate_fix_ = false;
  if (!pvt_data.flags.bits.gnssFixOK || !isNavigationFix(pvt_data.fixType) ||
      pvt_data.flags3.bits.invalidLlh ||
      !coordinatesAreInRange(pvt_data.lat, pvt_data.lon)) {
    return;
  }

  // iTOW is not monotonic across GPS week rollover. Once another PVT epoch is
  // observed, an equal iTOW from a future week must remain eligible.
  if (has_last_promoted_fix_itow_ &&
      pvt_data.iTOW != last_promoted_fix_itow_) {
    has_last_promoted_fix_itow_ = false;
  }

  candidate_fix_.latitude_e7 = pvt_data.lat;
  candidate_fix_.longitude_e7 = pvt_data.lon;
  candidate_fix_.altitude_mm = pvt_data.height;
  candidate_fix_.satellites = pvt_data.numSV;
  candidate_fix_.flags = tlp::kPositionFlagValidFix;
  candidate_fix_.utc_epoch_seconds = 0;
  if (pvt_data.valid.bits.validDate && pvt_data.valid.bits.validTime) {
    candidate_fix_.flags |= tlp::kPositionFlagValidUtcTime;
    candidate_fix_.utc_epoch_seconds = gnss.getUnixEpoch(0);
  }
  if (is3dFix(pvt_data.fixType)) {
    candidate_fix_.flags |= tlp::kPositionFlag3dFix;
  }

  candidate_fix_itow_ = pvt_data.iTOW;
  has_candidate_fix_ = true;
  considerPositionFix();
}

void GnssManager::handleDop(const UBX_NAV_DOP_data_t& dop_data) {
  latest_hdop_itow_ = dop_data.iTOW;
  latest_hdop_x100_ = dop_data.hDOP;
  has_latest_hdop_ = true;
  considerPositionFix();
}

void GnssManager::considerPositionFix() {
  if (!has_candidate_fix_ || !has_latest_hdop_ ||
      candidate_fix_itow_ != latest_hdop_itow_ || fresh_fix_ready_ ||
      (has_last_promoted_fix_itow_ &&
       candidate_fix_itow_ == last_promoted_fix_itow_)) {
    return;
  }

  candidate_fix_.hdop_x100 = latest_hdop_x100_;
  if (has_sent_position_ && millis() - last_position_tx_at_ms_ <
      gnss_config::kPositionTestIntervalMs) {
    return;
  }

  fresh_fix_ = candidate_fix_;
  fresh_fix_itow_ = candidate_fix_itow_;
  fresh_fix_ready_ = true;
  last_promoted_fix_itow_ = fresh_fix_itow_;
  has_last_promoted_fix_itow_ = true;
  Serial.printf("GNSS FIX lat=%ld lon=%ld sats=%u hdop=%u.%02u\n",
                static_cast<long>(fresh_fix_.latitude_e7),
                static_cast<long>(fresh_fix_.longitude_e7),
                fresh_fix_.satellites, fresh_fix_.hdop_x100 / 100,
                fresh_fix_.hdop_x100 % 100);
}

}  // namespace orun_tlp
