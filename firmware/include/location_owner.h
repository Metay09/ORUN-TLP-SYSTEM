#pragma once

#include <stdint.h>

namespace orun_tlp {

enum class LocationSource : uint8_t {
  kUnknown = 0,
  kGnss = 1,
};

struct AcceptedLocation {
  uint64_t observed_monotonic_ms = 0;
  uint32_t utc_epoch_seconds = 0;
  int32_t latitude_e7 = 0;
  int32_t longitude_e7 = 0;
  int32_t altitude_mm = 0;
  LocationSource source = LocationSource::kUnknown;
  bool altitude_valid = false;
  bool utc_valid = false;
};

constexpr uint64_t extendRecentMonotonicMs(uint64_t now_ms,
                                           uint32_t captured_at_ms_mod32) {
  return now_ms - static_cast<uint64_t>(
                      static_cast<uint32_t>(now_ms) - captured_at_ms_mod32);
}

class LocationOwner {
 public:
  bool hasLocation() const { return has_location_; }

  bool latest(AcceptedLocation* output) const {
    if (!has_location_ || output == nullptr) return false;
    *output = latest_;
    return true;
  }

  bool accept(const AcceptedLocation& candidate) {
    if (candidate.source == LocationSource::kUnknown ||
        candidate.latitude_e7 < -900000000 ||
        candidate.latitude_e7 > 900000000 ||
        candidate.longitude_e7 < -1800000000 ||
        candidate.longitude_e7 > 1800000000) {
      return false;
    }
    latest_ = candidate;
    has_location_ = true;
    return true;
  }

 private:
  AcceptedLocation latest_{};
  bool has_location_ = false;
};

static_assert(sizeof(AcceptedLocation) <= 32,
              "AcceptedLocation must remain bounded");

}  // namespace orun_tlp
