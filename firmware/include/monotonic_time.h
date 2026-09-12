#pragma once

#include <stdint.h>

namespace orun_tlp::monotonic {

// Deadlines and maximum time between observations must be less than 2^31 ms.
// Unsigned subtraction is intentional, including across UINT32_MAX -> 0.
constexpr bool reached(uint32_t now, uint32_t deadline) {
  return uint32_t(now - deadline) < 0x80000000UL;
}

constexpr bool elapsed(uint32_t now, uint32_t start, uint32_t duration) {
  return uint32_t(now - start) >= duration;
}

// Keep the original schedule phase. Skip all due points, including now, in
// constant time; never emit a catch-up acquisition for each missed interval.
constexpr uint32_t nextFuture(uint32_t now, uint32_t due, uint32_t interval) {
  return reached(now, due)
             ? due + (uint32_t(now - due) / interval + 1) * interval
             : due;
}

// Adafruit nRF52 1.7.0 millis() converts a wrapping 1024 Hz tick count to ms.
// Extend ticks BEFORE conversion so our ms clock wraps at exactly 2^32 ms.
// Called from the cooperative loop only, at least once per tick-counter wrap.
class TickMillis {
 public:
  uint32_t update(uint32_t ticks, uint32_t ticks_per_second) {
    extended_ticks_ += uint32_t(ticks - previous_ticks_);
    previous_ticks_ = ticks;
    return static_cast<uint32_t>(extended_ticks_ * 1000 / ticks_per_second);
  }

 private:
  uint64_t extended_ticks_ = 0;
  uint32_t previous_ticks_ = 0;
};

uint32_t nowMs();  // Loop task only; not an ISR or cross-task clock.

static_assert(reached(5, UINT32_MAX - 5), "deadline across rollover");
static_assert(!reached(UINT32_MAX - 5, 5), "future across rollover");
static_assert(nextFuture(35000, 10000, 10000) == 40000, "skip missed slots");
static_assert(nextFuture(30000, 10000, 10000) == 40000, "strictly future");
static_assert(nextFuture(5, UINT32_MAX - 4, 10) == 15, "wrapped schedule");

}  // namespace orun_tlp::monotonic
