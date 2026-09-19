// M7P7B: pure BLE admission policy checks, against the exact tracker policy
// in docs/architecture/ORUN_FIELD_NETWORK_DIAGNOSTICS_PLAN.md §10.
#include <assert.h>
#include <stdio.h>

#include "ble_admission_policy.h"

using namespace orun_tlp;
using namespace orun_tlp::ble_admission_config;

int main() {
  // 1. Opens at begin(); no premature close before the window elapses.
  {
    BleAdmissionPolicy policy;
    policy.begin(0);
    assert(policy.isOpen());
    assert(!policy.isConnected());
    assert(policy.update(false, kNoClientTimeoutMs - 1) == BleAdmissionAction::kNone);
    assert(policy.isOpen());
  }

  // 2. No client connects before the ~10-minute deadline: closes exactly
  // once, on the tick the deadline is reached.
  {
    BleAdmissionPolicy policy;
    policy.begin(0);
    assert(policy.update(false, kNoClientTimeoutMs) == BleAdmissionAction::kClose);
    assert(!policy.isOpen());
    // Stays closed; does not reopen itself on later ticks.
    assert(policy.update(false, kNoClientTimeoutMs + 1) == BleAdmissionAction::kNone);
    assert(!policy.isOpen());
    assert(policy.update(true, kNoClientTimeoutMs + 2) == BleAdmissionAction::kNone);
    assert(!policy.isOpen());
  }

  // 3. A connected client suspends the timeout, even past the original
  // deadline -- BLE must not close while connected.
  {
    BleAdmissionPolicy policy;
    policy.begin(0);
    assert(policy.update(true, kNoClientTimeoutMs / 2) == BleAdmissionAction::kNone);
    assert(policy.isOpen());
    assert(policy.isConnected());
    // Long past the original deadline, still connected: still open.
    assert(policy.update(true, kNoClientTimeoutMs * 10) == BleAdmissionAction::kNone);
    assert(policy.isOpen());
  }

  // 4. After disconnect, a fresh ~10-minute window starts (not measured from
  // the original boot time).
  {
    BleAdmissionPolicy policy;
    policy.begin(0);
    const uint32_t connect_at = kNoClientTimeoutMs / 2;
    assert(policy.update(true, connect_at) == BleAdmissionAction::kNone);
    const uint32_t disconnect_at = kNoClientTimeoutMs * 5;  // long past original deadline
    assert(policy.update(false, disconnect_at) == BleAdmissionAction::kNone);
    assert(policy.isOpen());
    assert(!policy.isConnected());
    // Original deadline (kNoClientTimeoutMs from boot) has long passed, but
    // the fresh window (from disconnect_at) has not: must still be open.
    assert(policy.update(false, disconnect_at + kNoClientTimeoutMs - 1) ==
           BleAdmissionAction::kNone);
    assert(policy.isOpen());
    // Fresh window itself now elapses: closes.
    assert(policy.update(false, disconnect_at + kNoClientTimeoutMs) ==
           BleAdmissionAction::kClose);
    assert(!policy.isOpen());
  }

  // 5. Repeated connect/disconnect cycles must not create permanent
  // availability: each disconnect grants exactly one bounded fresh window,
  // and if nothing reconnects within THAT window, it closes.
  {
    BleAdmissionPolicy policy;
    policy.begin(0);
    uint32_t now = 0;
    for (int cycle = 0; cycle < 5; ++cycle) {
      now += 1000;
      assert(policy.update(true, now) == BleAdmissionAction::kNone);  // connect
      now += 1000;
      assert(policy.update(false, now) == BleAdmissionAction::kNone);  // disconnect
      assert(policy.isOpen());
    }
    // After the last disconnect, nobody reconnects: the final bounded
    // window still closes on schedule, not extended by the earlier cycles.
    assert(policy.update(false, now + kNoClientTimeoutMs) == BleAdmissionAction::kClose);
    assert(!policy.isOpen());
  }

  // 6. Reconnect during the renewed (post-disconnect) window works, and
  // suspends that window's own deadline in turn.
  {
    BleAdmissionPolicy policy;
    policy.begin(0);
    assert(policy.update(true, 100) == BleAdmissionAction::kNone);
    assert(policy.update(false, 200) == BleAdmissionAction::kNone);  // fresh window from 200
    // Reconnect well within the fresh window, past where the ORIGINAL boot
    // deadline would have fired.
    assert(policy.update(true, kNoClientTimeoutMs) == BleAdmissionAction::kNone);
    assert(policy.isOpen());
    assert(policy.isConnected());
    assert(policy.update(true, kNoClientTimeoutMs * 3) == BleAdmissionAction::kNone);
    assert(policy.isOpen());
  }

  puts("M7P7B BLE admission policy checks: PASS");
}
