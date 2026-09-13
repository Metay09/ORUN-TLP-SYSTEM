#pragma once

#include <stdint.h>

namespace orun_tlp {

struct UtcSnapshot {
  uint16_t year;
  uint8_t month, day, hour, minute, second;
};

// Gregorian UTC, uint32 wire range. No timezone, locale or allocation.
// u-blox sec=60 is normalized into the following minute, as in 2.2.29.
bool utcToEpoch(const UtcSnapshot& utc, uint32_t& epoch);

}  // namespace orun_tlp
