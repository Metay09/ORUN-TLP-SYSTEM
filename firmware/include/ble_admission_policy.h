#pragma once

#include <stdint.h>

namespace orun_tlp {

namespace ble_admission_config {
// docs/architecture/ORUN_FIELD_NETWORK_DIAGNOSTICS_PLAN.md §10 / AGENTS.md
// "BLE" section: tracker boot/no-client BLE availability window.
constexpr uint32_t kNoClientTimeoutMs = 10UL * 60UL * 1000UL;  // ~10 minutes
// Minimum spacing between repeated physical operations the caller reported
// as failed or unconfirmed (Advertising.stop() / Advertising.start()). Keeps
// a persistent failure from busy-spinning the cooperative loop.
constexpr uint32_t kRetryIntervalMs = 1000UL;
}  // namespace ble_admission_config

enum class BleAdmissionAction : uint8_t {
  kNone,
  // Caller must try to stop advertising now and, only once it has observed
  // advertising not running with no client connected, call confirmClosed().
  // Repeated (throttled) until confirmed or a client wins the race.
  kClose,
  // Window is open, no client is connected, but advertising is not running:
  // caller must try Advertising.start(0). Repeated (throttled) until
  // advertising is observed running or the window expires.
  kStartAdvertising,
};

// One loop-tick's worth of observed facts. All fields are sampled by the loop
// task; none is written from a Bluefruit callback.
struct BleAdmissionInput {
  // Actual Periph.connected() > 0 at sampling time.
  bool connected = false;
  // At least one real Bluefruit disconnect event was handed off since the
  // previous tick (see main.cpp). Covers a connect+disconnect that happened
  // entirely between two polls.
  bool disconnect_event = false;
  // Actual Advertising.isRunning() at sampling time.
  bool advertising_running = false;
};

// M7P7B: pure BLE availability admission policy. Decides WHEN the tracker's
// BLE availability window should close and whether advertising must be
// (re)started while it is open; it knows nothing about Bluefruit, SoftDevice,
// advertising mechanics, connection identity, GATT, bonding or authorization
// -- see docs/architecture/ORUN_FIELD_NETWORK_DIAGNOSTICS_PLAN.md §10 for the
// product policy this implements and docs/milestones/M7P7B.md for how the
// runtime glue (main.cpp) drives it from Bluefruit.
//
// Requesting close and confirming physical close are separate: the deadline
// only moves the policy to "closing"; it becomes permanently closed only when
// the caller calls confirmClosed() after observing advertising stopped.
//
// Single-connected-client model only, matching this milestone's scope. Not
// safe to call from more than one task; drive it from the same cooperative
// loop task that owns monotonic::nowMs() (see monotonic_time.h).
class BleAdmissionPolicy {
 public:
  // now: boot/open time. Opens a fresh no-client window immediately. Call
  // only after the boot-time Advertising.start(0) succeeded; without it the
  // policy stays closed (fail-closed).
  void begin(uint32_t now);

  // Call every loop tick. Returns the physical action the caller must try.
  BleAdmissionAction update(const BleAdmissionInput& input, uint32_t now);

  // Caller observed advertising not running with no client connected after a
  // kClose. Ignored unless a close is pending, so a client that connected
  // meanwhile (update() moved back to open) is never closed.
  void confirmClosed();

  // Window is open (advertising is wanted, or a client is connected).
  bool isOpen() const { return state_ == State::kOpen; }
  // Deadline reached; physical stop not yet confirmed.
  bool isClosing() const { return state_ == State::kClosing; }
  bool isClosed() const { return state_ == State::kClosed; }
  // True only once update() has last been called with connected == true.
  bool isConnected() const { return connected_; }

 private:
  enum class State : uint8_t { kClosed, kOpen, kClosing };

  void startFreshWindow(uint32_t now);

  State state_ = State::kClosed;
  bool connected_ = false;
  // Meaningful only while kOpen && !connected_. Absolute deadline: the tick
  // at or after which, if still disconnected, close is requested.
  uint32_t close_at_ms_ = 0;
  // Earliest tick at which the next kClose / kStartAdvertising may be
  // emitted (wrap-safe via monotonic::reached).
  uint32_t next_action_at_ms_ = 0;
};

}  // namespace orun_tlp
