#pragma once

#include <stdint.h>

#include <SparkFun_u-blox_GNSS_Arduino_Library.h>

namespace orun_tlp {

struct GnssFix {
  uint32_t utc_epoch_seconds;
  int32_t latitude_e7;
  int32_t longitude_e7;
  int32_t altitude_mm;
  uint16_t hdop_x100;
  uint8_t satellites;
  uint8_t flags;
};

class GnssManager {
 public:
  enum class State : uint8_t {
    kNotPresent, kPowerOff, kPowerOnWait, kDetecting, kIdle, kStarting,
    kAcquiring, kFixAvailable, kTimeout, kFailure, kSleeping
  };

  struct Diagnostics {
    uint32_t acquisition_attempts = 0;
    uint32_t successful_fresh_fixes = 0;
    uint32_t acquisition_timeouts = 0;
    uint32_t invalid_fixes = 0;
    uint32_t configuration_failures = 0;
    uint32_t expired_unsent_fixes = 0;
    uint32_t last_ttff_ms = 0;
  };

  void begin();
  void poll();
  bool takeFreshFixForTransmission(GnssFix* fix);
  bool detected() const;
  bool detectionComplete() const {
    return state_ != State::kPowerOff && state_ != State::kPowerOnWait &&
           state_ != State::kDetecting;
  }
  State state() const { return state_; }
  const Diagnostics& diagnostics() const { return diagnostics_; }

 private:
  static void onPvt(UBX_NAV_PVT_data_t* pvt_data);
  static void onDop(UBX_NAV_DOP_data_t* dop_data);
  void handlePvt(const UBX_NAV_PVT_data_t& pvt_data);
  void handleDop(const UBX_NAV_DOP_data_t& dop_data);
  void considerPositionFix();
  void startAcquisition(uint32_t now);
  void prepareAcquisition();
  void enterLowPower(uint32_t now);
  void clearCandidates();
  void expireFreshFix(uint32_t now);

  State state_ = State::kNotPresent;
  Diagnostics diagnostics_{};
  bool detected_ = false;
  bool needs_configuration_ = true;
  bool waiting_for_power_ = false;
  uint8_t configuration_step_ = 0;
  uint32_t state_changed_at_ms_ = 0;
  uint32_t acquisition_started_at_ms_ = 0;
  uint32_t next_due_at_ms_ = 0;
  uint32_t fresh_fix_at_ms_ = 0;
  bool has_boundary_epoch_ = false;
  uint32_t boundary_epoch_ = 0;
  uint32_t latest_hdop_itow_ = 0;
  uint16_t latest_hdop_x100_ = 0;
  bool has_latest_hdop_ = false;
  GnssFix candidate_fix_{};
  uint32_t candidate_fix_itow_ = 0;
  bool has_candidate_fix_ = false;
  GnssFix fresh_fix_{};
  uint32_t fresh_fix_itow_ = 0;
  bool fresh_fix_ready_ = false;
  uint32_t last_promoted_fix_itow_ = 0;
  bool has_last_promoted_fix_itow_ = false;
};

}  // namespace orun_tlp
