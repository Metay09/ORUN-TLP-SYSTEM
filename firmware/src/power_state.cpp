#include "power_state.h"

#include "monotonic_time.h"

namespace orun_tlp {

const char* powerStateName(PowerState state) {
  switch (state) {
    case PowerState::kBattery: return "BATTERY";
    case PowerState::kCharging: return "CHARGING";
    case PowerState::kFull: return "FULL";
    case PowerState::kNoBattery: return "NO_BATTERY";
    case PowerState::kUnknown: break;
  }
  return "UNKNOWN";
}

bool PowerStateTracker::risingOver(uint32_t now_ms, uint32_t mv) const {
  // Against the newest kept reading at least the window old; with less
  // history, against the oldest one if it is at least half the window old.
  const Sample* base = nullptr;
  for (uint8_t i = 0; i < count_; ++i) {
    const Sample& s = samples_[(head_ + kSamples - 1 - i) % kSamples];   // newest first
    if (monotonic::elapsed(now_ms, s.at_ms, power_config::kRiseWindowMs)) { base = &s; break; }
  }
  if (base == nullptr && count_ != 0) {
    const Sample& oldest = samples_[(head_ + kSamples - count_) % kSamples];
    if (monotonic::elapsed(now_ms, oldest.at_ms, power_config::kRiseWindowMs / 2)) base = &oldest;
  }
  return base != nullptr &&
         static_cast<int32_t>(mv) - static_cast<int32_t>(base->mv) >= power_config::kRiseMv;
}

uint32_t PowerStateTracker::maxRecent(uint8_t n) const {
  uint32_t best = 0;
  for (uint8_t i = 0; i < n && i < count_; ++i) {
    const Sample& s = samples_[(head_ + kSamples - 1 - i) % kSamples];
    if (s.mv > best) best = s.mv;
  }
  return best;
}

bool PowerStateTracker::takeChargeEndPeak(uint32_t& millivolts) {
  if (!peak_ready_) return false;
  peak_ready_ = false;
  millivolts = peak_mv_;
  return true;
}

bool PowerStateTracker::update(uint32_t now_ms, uint32_t mv, bool usb) {
  using namespace power_config;
  const PowerState before = state_;
  if (usb != usb_) {
    ended_ = false;   // a new supply session: its own charge end
    high_ = false;
  }
  usb_ = usb;

  const bool have_prev = count_ != 0;
  const uint32_t prev = have_prev ? samples_[(head_ + kSamples - 1) % kSamples].mv : mv;
  // Climbing before this reading (the window up to the previous one).
  const bool was_rising = have_prev && (before == PowerState::kCharging || risingOver(now_ms, prev));
  const bool drop = have_prev && static_cast<int32_t>(prev) - static_cast<int32_t>(mv) >= kEndDropMv;
  if (drop && was_rising && prev >= kEndPeakMinMv) {
    ended_ = true;
    peak_mv_ = maxRecent(3);
    peak_ready_ = true;
    ++charge_ends_;
  }

  samples_[head_] = Sample{now_ms, mv};
  head_ = static_cast<uint8_t>((head_ + 1) % kSamples);
  if (count_ < kSamples) ++count_;

  if (usb && mv >= kNoBatteryMv) {
    if (!high_) { high_ = true; high_since_ms_ = now_ms; }
  } else {
    high_ = false;
  }
  const bool rising = risingOver(now_ms, mv);

  if (usb && high_ && monotonic::elapsed(now_ms, high_since_ms_, kNoBatteryHoldMs)) {
    state_ = PowerState::kNoBattery;
  } else if (usb && high_ && !ended_) {
    // The charger's 4.2 V, with or without a cell behind it: not known yet.
    state_ = PowerState::kUnknown;
  } else if (usb) {
    if (ended_ && mv >= kHoldLowMv) state_ = PowerState::kFull;
    else if (before == PowerState::kFull && mv >= kHoldLowMv && !rising) state_ = PowerState::kFull;
    else if (rising || mv < kFloatLowMv) state_ = PowerState::kCharging;
    else if (count_ >= 3) state_ = PowerState::kFull;   // steady in the float band
    else state_ = PowerState::kCharging;
  } else {
    // No USB: a solar panel, if any, shows only in the voltage.
    if (ended_ && mv >= kSolarFullLowMv) state_ = PowerState::kFull;
    else if (rising) state_ = PowerState::kCharging;
    else state_ = PowerState::kBattery;
    if (mv < kSolarFullLowMv) ended_ = false;
  }
  return state_ != before;
}

}  // namespace orun_tlp
