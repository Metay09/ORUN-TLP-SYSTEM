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
  void begin();
  void poll();
  bool takeFreshFixForTransmission(GnssFix* fix);
  bool detected() const;

 private:
  enum class State : uint8_t { kPowerOff, kPowerOnWait, kDetecting, kReady, kUnavailable };

  static void onPvt(UBX_NAV_PVT_data_t* pvt_data);
  static void onDop(UBX_NAV_DOP_data_t* dop_data);
  void handlePvt(const UBX_NAV_PVT_data_t& pvt_data);
  void handleDop(const UBX_NAV_DOP_data_t& dop_data);
  void considerPositionFix();

  State state_ = State::kUnavailable;
  uint32_t state_changed_at_ms_ = 0;
  uint32_t last_position_tx_at_ms_ = 0;
  uint32_t latest_hdop_itow_ = 0;
  uint16_t latest_hdop_x100_ = 0;
  bool has_latest_hdop_ = false;
  bool has_sent_position_ = false;
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
