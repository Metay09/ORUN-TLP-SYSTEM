#pragma once

#include <stdint.h>

namespace orun_tlp {

// Pil 2 (owner 2026-11-12, claude/ORUN_PIL_OLCUM.md): what the battery is
// doing, from the quiet battery readings (one a minute) and the USB supply.
// The RAK19007 charger (TP4054) tells the MCU nothing, so this is read from
// the voltage, like RNode does on boards without a PMU:
//
// - CHARGING: the voltage climbs (USB, or a solar panel by day).
// - FULL: the charger ended its cycle. That shows as a climb that suddenly
//   drops (>= 15 mV in a minute) near 4.2 V (bench 2026-11-12: 4175 -> 4148);
//   the charger then holds the cell between its 4.05 V restart threshold and
//   4.2 V, which also counts as full while USB stays.
// - NO_BATTERY: USB, and the reading never leaves the charger's open output
//   (>= 4.23 V for two hours; a real cell falls back after its charge ends).
// - BATTERY: none of these.
//
// The drop also gives a reference: at the end of its constant-voltage phase
// the charger holds the cell at 4.20 V (+-1 %). The reading just before the
// drop is reported so the monitor can learn this board's scale.
enum class PowerState : uint8_t {
  kUnknown = 0,
  kBattery = 1,
  kCharging = 2,
  kFull = 3,
  kNoBattery = 4,
};

const char* powerStateName(PowerState state);

namespace power_config {
constexpr uint32_t kRiseWindowMs = 10UL * 60UL * 1000UL;
constexpr int32_t kRiseMv = 8;                   // over the window: climbing
constexpr int32_t kEndDropMv = 15;               // in one reading: charge ended
constexpr uint32_t kEndPeakMinMv = 4080;         // a drop below this is not the end
constexpr uint32_t kHoldLowMv = 4000;            // full stays full above this (USB)
constexpr uint32_t kFloatLowMv = 4030;           // USB, steady and above: full
constexpr uint32_t kSolarFullLowMv = 4050;       // no USB: full until the restart threshold
constexpr uint32_t kNoBatteryMv = 4230;
constexpr uint32_t kNoBatteryHoldMs = 2UL * 60UL * 60UL * 1000UL;
constexpr uint32_t kChargerFloatMv = 4200;       // TP4054 VFLOAT, +-1 %
}  // namespace power_config

class PowerStateTracker {
 public:
  // One quiet reading (scaled millivolts) and the USB supply at that moment.
  // Returns true when the state changed.
  bool update(uint32_t now_ms, uint32_t millivolts, bool usb);

  PowerState state() const { return state_; }
  // A charge end was just seen: the reading before the drop (the charger's
  // 4.20 V), in the millivolts given to update(). Taken once.
  bool takeChargeEndPeak(uint32_t& millivolts);
  // The ADC offset was just calibrated: readings before it are not
  // comparable with the next one (a step of up to ~20 mV). Forget them so a
  // step is neither a climb nor a charge end; the state itself is kept.
  void rebase();
  uint32_t chargeEnds() const { return charge_ends_; }

 private:
  struct Sample { uint32_t at_ms; uint32_t mv; };
  static constexpr uint8_t kSamples = 16;
  bool risingOver(uint32_t now_ms, uint32_t mv) const;
  uint32_t maxRecent(uint8_t n) const;

  Sample samples_[kSamples]{};
  uint8_t count_ = 0;
  uint8_t head_ = 0;   // next write
  PowerState state_ = PowerState::kUnknown;
  bool usb_ = false;
  bool ended_ = false;           // a charge end seen while power stayed
  uint32_t high_since_ms_ = 0;   // USB and >= kNoBatteryMv since
  bool high_ = false;
  bool peak_ready_ = false;
  uint32_t peak_mv_ = 0;
  uint32_t charge_ends_ = 0;
};

}  // namespace orun_tlp
