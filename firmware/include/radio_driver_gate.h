#pragma once
#include <stdint.h>

// Single physical radio; initialize before library task creation. Task context
// only. No callback acquires this gate: dispatch already holds it.
namespace orun_tlp::radio_driver {
bool initialize();
bool tryAcquire();
void acquire();
void release();
uint32_t generation();             // Gate held, immutable through dispatch.
void setGeneration(uint32_t value); // Gate held, after quiesce only.
class Guard {
 public:
  explicit Guard(bool wait = false) {
    if (wait) { acquire(); held_ = true; }
    else held_ = tryAcquire();
  }
  ~Guard() { if (held_) release(); }
  explicit operator bool() const { return held_; }
  Guard(const Guard&) = delete;
  Guard& operator=(const Guard&) = delete;
 private:
  bool held_ = false;
};
}  // namespace orun_tlp::radio_driver

// Supplied by the pinned dependency patch. Caller must hold gate. The raw
// dispatch entry avoids recursive locking when owner drains pending IRQs.
void orunRadioDispatchLocked();
void orunRadioTimeoutLocked();
void orunRadioQuiesceLocked();
