#pragma once

#include <stdint.h>

namespace orun_tlp {

namespace ble_admission_config {
// docs/architecture/ORUN_FIELD_NETWORK_DIAGNOSTICS_PLAN.md §10 / AGENTS.md
// "BLE" section: tracker boot/no-client BLE availability window.
constexpr uint32_t kNoClientTimeoutMs = 10UL * 60UL * 1000UL;  // ~10 minutes
}  // namespace ble_admission_config

enum class BleAdmissionAction : uint8_t {
  kNone,
  // Caller must stop advertising/close BLE now. Returned exactly once, on
  // the tick the no-client window expires.
  kClose,
};

// M7P7B: pure BLE availability admission policy. Decides only WHEN the
// tracker's BLE availability window should close; it knows nothing about
// Bluefruit, SoftDevice, advertising mechanics, connection identity, GATT,
// bonding or authorization -- see
// docs/architecture/ORUN_FIELD_NETWORK_DIAGNOSTICS_PLAN.md §10 for the
// product policy this implements and docs/milestones/M7P7B.md for how the
// runtime glue (main.cpp) drives it from Bluefruit.
//
// Single-connected-client model only, matching this milestone's scope: the
// caller collapses "any client connected" into one bool before calling
// update(). Not safe to call from more than one task; drive it from the
// same cooperative loop task that owns monotonic::nowMs() (see
// monotonic_time.h).
class BleAdmissionPolicy {
 public:
  // now: boot/open time. Opens a fresh no-client window immediately.
  void begin(uint32_t now);

  // Call every loop tick with the current connection state and time.
  // Returns kClose exactly once, on the tick the window should close;
  // kNone otherwise, including every tick after close.
  BleAdmissionAction update(bool connected, uint32_t now);

  // True from begin() until update() returns kClose. False before begin()
  // and after close.
  bool isOpen() const { return open_; }
  // True only once update() has last been called with connected == true.
  bool isConnected() const { return connected_; }

 private:
  bool open_ = false;
  bool connected_ = false;
  // Meaningful only while open_ && !connected_. Absolute deadline: the tick
  // at or after which, if still disconnected, the window closes.
  uint32_t close_at_ms_ = 0;
};

}  // namespace orun_tlp
