// M7P7B: pure BLE admission policy checks, against the exact tracker policy
// in docs/architecture/ORUN_FIELD_NETWORK_DIAGNOSTICS_PLAN.md §10.
#include <assert.h>
#include <stdio.h>

#include "ble_admission_policy.h"

using namespace orun_tlp;
using namespace orun_tlp::ble_admission_config;

namespace {
// Legacy-shaped tick: polled connection state only, advertising healthy.
BleAdmissionAction upd(BleAdmissionPolicy& p, bool connected, uint32_t now) {
  return p.update({connected, false, true}, now);
}
// Tick carrying a handed-off real disconnect event.
BleAdmissionAction updEvent(BleAdmissionPolicy& p, bool connected, uint32_t now,
                            bool advertising_running = true) {
  return p.update({connected, true, advertising_running}, now);
}
BleAdmissionAction updAdv(BleAdmissionPolicy& p, bool connected, uint32_t now,
                          bool advertising_running) {
  return p.update({connected, false, advertising_running}, now);
}
}  // namespace

int main() {
  // 1. Opens at begin(); no premature close before the window elapses.
  {
    BleAdmissionPolicy policy;
    policy.begin(0);
    assert(policy.isOpen());
    assert(!policy.isConnected());
    assert(upd(policy, false, kNoClientTimeoutMs - 1) == BleAdmissionAction::kNone);
    assert(policy.isOpen());
  }

  // 2. No client connects before the ~10-minute deadline: closes exactly
  // once, on the tick the deadline is reached.
  {
    BleAdmissionPolicy policy;
    policy.begin(0);
    assert(upd(policy, false, kNoClientTimeoutMs) == BleAdmissionAction::kClose);
    // Close is requested, not yet confirmed physically.
    assert(!policy.isOpen() && policy.isClosing());
    policy.confirmClosed();
    assert(!policy.isOpen() && policy.isClosed());
    // Stays closed; does not reopen itself on later ticks.
    assert(upd(policy, false, kNoClientTimeoutMs + 1) == BleAdmissionAction::kNone);
    assert(!policy.isOpen());
    assert(upd(policy, true, kNoClientTimeoutMs + 2) == BleAdmissionAction::kNone);
    assert(!policy.isOpen());
  }

  // 3. A connected client suspends the timeout, even past the original
  // deadline -- BLE must not close while connected.
  {
    BleAdmissionPolicy policy;
    policy.begin(0);
    assert(upd(policy, true, kNoClientTimeoutMs / 2) == BleAdmissionAction::kNone);
    assert(policy.isOpen());
    assert(policy.isConnected());
    // Long past the original deadline, still connected: still open.
    assert(upd(policy, true, kNoClientTimeoutMs * 10) == BleAdmissionAction::kNone);
    assert(policy.isOpen());
  }

  // 4. After disconnect, a fresh ~10-minute window starts (not measured from
  // the original boot time).
  {
    BleAdmissionPolicy policy;
    policy.begin(0);
    const uint32_t connect_at = kNoClientTimeoutMs / 2;
    assert(upd(policy, true, connect_at) == BleAdmissionAction::kNone);
    const uint32_t disconnect_at = kNoClientTimeoutMs * 5;  // long past original deadline
    assert(upd(policy, false, disconnect_at) == BleAdmissionAction::kNone);
    assert(policy.isOpen());
    assert(!policy.isConnected());
    // Original deadline (kNoClientTimeoutMs from boot) has long passed, but
    // the fresh window (from disconnect_at) has not: must still be open.
    assert(upd(policy, false, disconnect_at + kNoClientTimeoutMs - 1) ==
           BleAdmissionAction::kNone);
    assert(policy.isOpen());
    // Fresh window itself now elapses: closes.
    assert(upd(policy, false, disconnect_at + kNoClientTimeoutMs) ==
           BleAdmissionAction::kClose);
    assert(!policy.isOpen() && policy.isClosing());
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
      assert(upd(policy, true, now) == BleAdmissionAction::kNone);  // connect
      now += 1000;
      assert(upd(policy, false, now) == BleAdmissionAction::kNone);  // disconnect
      assert(policy.isOpen());
    }
    // After the last disconnect, nobody reconnects: the final bounded
    // window still closes on schedule, not extended by the earlier cycles.
    assert(upd(policy, false, now + kNoClientTimeoutMs) == BleAdmissionAction::kClose);
    assert(!policy.isOpen() && policy.isClosing());
  }

  // 6. Reconnect during the renewed (post-disconnect) window works, and
  // suspends that window's own deadline in turn.
  {
    BleAdmissionPolicy policy;
    policy.begin(0);
    assert(upd(policy, true, 100) == BleAdmissionAction::kNone);
    assert(upd(policy, false, 200) == BleAdmissionAction::kNone);  // fresh window from 200
    // Reconnect well within the fresh window, past where the ORIGINAL boot
    // deadline would have fired.
    assert(upd(policy, true, kNoClientTimeoutMs) == BleAdmissionAction::kNone);
    assert(policy.isOpen());
    assert(policy.isConnected());
    assert(upd(policy, true, kNoClientTimeoutMs * 3) == BleAdmissionAction::kNone);
    assert(policy.isOpen());
  }

  // 7. Audit finding 1: a connection that starts and ends entirely between
  // two polls is never seen as connected, but the handed-off real disconnect
  // event still grants a fresh ~10-minute window.
  {
    BleAdmissionPolicy policy;
    policy.begin(0);
    const uint32_t before_deadline = kNoClientTimeoutMs - 1000;
    assert(upd(policy, false, before_deadline) == BleAdmissionAction::kNone);
    // Short session between polls: next poll shows connected=false again,
    // plus the event.
    const uint32_t event_at = before_deadline + 500;
    assert(updEvent(policy, false, event_at) == BleAdmissionAction::kNone);
    assert(policy.isOpen() && !policy.isConnected());
    // The ORIGINAL deadline passes without closing...
    assert(upd(policy, false, kNoClientTimeoutMs + 5000) == BleAdmissionAction::kNone);
    assert(policy.isOpen());
    // ...and the fresh window closes exactly kNoClientTimeoutMs after the event.
    assert(upd(policy, false, event_at + kNoClientTimeoutMs - 1) == BleAdmissionAction::kNone);
    assert(upd(policy, false, event_at + kNoClientTimeoutMs) == BleAdmissionAction::kClose);
    // Without the event the same timeline would have closed at the original
    // deadline (control).
    BleAdmissionPolicy control;
    control.begin(0);
    assert(upd(control, false, kNoClientTimeoutMs) == BleAdmissionAction::kClose);
  }

  // 8. Event and polled edge for the same disconnect (event arrives after the
  // poll already saw the edge, or in the same tick) never double-extend more
  // than one window from the later observation, and a stale event delivered
  // while a NEW session is connected does not disconnect it.
  {
    BleAdmissionPolicy policy;
    policy.begin(0);
    assert(upd(policy, true, 100) == BleAdmissionAction::kNone);
    assert(updEvent(policy, false, 200) == BleAdmissionAction::kNone);  // edge + event
    assert(policy.isOpen() && !policy.isConnected());
    // Reconnect, then a late event from the first session while connected.
    assert(upd(policy, true, 300) == BleAdmissionAction::kNone);
    assert(updEvent(policy, true, 310) == BleAdmissionAction::kNone);
    assert(policy.isConnected() && policy.isOpen());
    assert(upd(policy, true, kNoClientTimeoutMs * 4) == BleAdmissionAction::kNone);
    assert(policy.isOpen());
    // Real end of second session: fresh window from that disconnect.
    const uint32_t end2 = kNoClientTimeoutMs * 4 + 10;
    assert(upd(policy, false, end2) == BleAdmissionAction::kNone);
    assert(upd(policy, false, end2 + kNoClientTimeoutMs - 1) == BleAdmissionAction::kNone);
    assert(upd(policy, false, end2 + kNoClientTimeoutMs) == BleAdmissionAction::kClose);
  }

  // 9. Audit finding 2: kClose is a REQUEST. Until the caller confirms the
  // physical stop it stays pending, repeats throttled, and the policy is not
  // closed; a failed stop is retried, never abandoned.
  {
    BleAdmissionPolicy policy;
    policy.begin(0);
    assert(upd(policy, false, kNoClientTimeoutMs) == BleAdmissionAction::kClose);
    assert(policy.isClosing() && !policy.isOpen() && !policy.isClosed());
    // Caller's stop failed: no confirmation. Throttled: not again this tick
    // or before the retry interval...
    assert(upd(policy, false, kNoClientTimeoutMs) == BleAdmissionAction::kNone);
    assert(upd(policy, false, kNoClientTimeoutMs + kRetryIntervalMs - 1) ==
           BleAdmissionAction::kNone);
    // ...but repeated again, and again, while unconfirmed.
    assert(upd(policy, false, kNoClientTimeoutMs + kRetryIntervalMs) ==
           BleAdmissionAction::kClose);
    assert(upd(policy, false, kNoClientTimeoutMs + 2 * kRetryIntervalMs) ==
           BleAdmissionAction::kClose);
    assert(policy.isClosing());
    // Confirming without a pending close is ignored elsewhere; here it lands.
    policy.confirmClosed();
    assert(policy.isClosed() && !policy.isOpen() && !policy.isClosing());
    // After confirmed close it stays closed: no more actions, no reopen, an
    // event or a connection does not resurrect it.
    assert(upd(policy, false, kNoClientTimeoutMs * 2) == BleAdmissionAction::kNone);
    assert(updEvent(policy, false, kNoClientTimeoutMs * 2 + 1, false) ==
           BleAdmissionAction::kNone);
    assert(upd(policy, true, kNoClientTimeoutMs * 2 + 2) == BleAdmissionAction::kNone);
    assert(policy.isClosed());
  }

  // 10. confirmClosed() outside a pending close is a no-op (open window and
  // never-begun policy are not closable by a stray confirmation).
  {
    BleAdmissionPolicy policy;
    policy.confirmClosed();
    assert(policy.isClosed());  // never began: closed, fail-closed default
    policy.begin(0);
    policy.confirmClosed();
    assert(policy.isOpen());
  }

  // 11. Connection racing a pending close: the client wins. The session stays
  // admitted (no close while connected), close is cancelled even if the
  // caller's late confirmClosed() arrives, and the real disconnect grants a
  // fresh window.
  {
    BleAdmissionPolicy policy;
    policy.begin(0);
    assert(upd(policy, false, kNoClientTimeoutMs) == BleAdmissionAction::kClose);
    assert(policy.isClosing());
    // Stop failed because a client connected; next tick sees it.
    assert(upd(policy, true, kNoClientTimeoutMs + 5) == BleAdmissionAction::kNone);
    assert(policy.isOpen() && policy.isConnected() && !policy.isClosing());
    policy.confirmClosed();  // stray late confirmation
    assert(policy.isOpen());
    assert(upd(policy, true, kNoClientTimeoutMs * 20) == BleAdmissionAction::kNone);
    assert(policy.isOpen());
    const uint32_t end = kNoClientTimeoutMs * 20 + 7;
    assert(upd(policy, false, end) == BleAdmissionAction::kNone);
    assert(policy.isOpen());
    assert(upd(policy, false, end + kNoClientTimeoutMs) == BleAdmissionAction::kClose);
  }

  // 12. The racing connection also fully missed by polling (connect+disconnect
  // while close pending): the event alone cancels the close and grants a
  // fresh window.
  {
    BleAdmissionPolicy policy;
    policy.begin(0);
    assert(upd(policy, false, kNoClientTimeoutMs) == BleAdmissionAction::kClose);
    const uint32_t t = kNoClientTimeoutMs + 20;
    assert(updEvent(policy, false, t, false) == BleAdmissionAction::kStartAdvertising);
    assert(policy.isOpen() && !policy.isClosing());
    assert(upd(policy, false, t + kNoClientTimeoutMs - 1) == BleAdmissionAction::kNone);
    assert(upd(policy, false, t + kNoClientTimeoutMs) == BleAdmissionAction::kClose);
  }

  // 13. Audit finding 3: an open window with nobody connected and advertising
  // not running asks the caller to (re)start, throttled and repeated while the
  // window lasts; a running advertiser needs nothing.
  {
    BleAdmissionPolicy policy;
    policy.begin(0);
    assert(upd(policy, true, 1000) == BleAdmissionAction::kNone);
    // Disconnect: first restart is immediate.
    assert(updAdv(policy, false, 2000, false) == BleAdmissionAction::kStartAdvertising);
    // Restart failed (still not running): no busy retry...
    assert(updAdv(policy, false, 2000, false) == BleAdmissionAction::kNone);
    assert(updAdv(policy, false, 2000 + kRetryIntervalMs - 1, false) ==
           BleAdmissionAction::kNone);
    // ...bounded, repeated retry.
    assert(updAdv(policy, false, 2000 + kRetryIntervalMs, false) ==
           BleAdmissionAction::kStartAdvertising);
    assert(updAdv(policy, false, 2000 + 2 * kRetryIntervalMs, false) ==
           BleAdmissionAction::kStartAdvertising);
    // Retry succeeded: running, nothing more to do.
    assert(updAdv(policy, false, 2000 + 3 * kRetryIntervalMs, true) ==
           BleAdmissionAction::kNone);
    // Retries do not extend the window: it still ends kNoClientTimeoutMs after
    // the disconnect, even with advertising down the whole time.
    BleAdmissionPolicy down;
    down.begin(0);
    assert(upd(down, true, 1000) == BleAdmissionAction::kNone);
    assert(updAdv(down, false, 2000, false) == BleAdmissionAction::kStartAdvertising);
    uint32_t now = 2000;
    while (now + kRetryIntervalMs < 2000 + kNoClientTimeoutMs) {
      now += kRetryIntervalMs;
      assert(updAdv(down, false, now, false) == BleAdmissionAction::kStartAdvertising);
    }
    // Expiry: desired state is closed, not another start.
    assert(updAdv(down, false, 2000 + kNoClientTimeoutMs, false) == BleAdmissionAction::kClose);
    assert(down.isClosing());
  }

  // 14. A never-begun policy (initial Advertising.start(0) failed) stays
  // closed: no window, no start, no close requests.
  {
    BleAdmissionPolicy policy;
    for (uint32_t t = 0; t < 3 * kNoClientTimeoutMs; t += kNoClientTimeoutMs / 3) {
      assert(updAdv(policy, false, t, false) == BleAdmissionAction::kNone);
      assert(policy.isClosed());
    }
    assert(updEvent(policy, false, 5, false) == BleAdmissionAction::kNone);
    assert(policy.isClosed());
  }

  // 15. UINT32 wrap safety: window across the 2^32 ms rollover, connected
  // session across it, fresh window, close retry and start retry.
  {
    const uint32_t base = UINT32_MAX - 1000;  // deadline wraps past zero
    BleAdmissionPolicy policy;
    policy.begin(base);
    assert(upd(policy, false, base + kNoClientTimeoutMs - 1) == BleAdmissionAction::kNone);
    assert(policy.isOpen());
    assert(upd(policy, false, base + kNoClientTimeoutMs) == BleAdmissionAction::kClose);
    assert(upd(policy, false, base + kNoClientTimeoutMs + kRetryIntervalMs - 1) ==
           BleAdmissionAction::kNone);
    assert(upd(policy, false, base + kNoClientTimeoutMs + kRetryIntervalMs) ==
           BleAdmissionAction::kClose);

    BleAdmissionPolicy wrapped;
    wrapped.begin(base);
    assert(upd(wrapped, true, base + 500) == BleAdmissionAction::kNone);
    const uint32_t rolled = base + 5000;  // numerically small after wrap
    assert(rolled < base);
    assert(upd(wrapped, true, rolled) == BleAdmissionAction::kNone);
    assert(updAdv(wrapped, false, rolled + 10, false) == BleAdmissionAction::kStartAdvertising);
    assert(updAdv(wrapped, false, rolled + 10 + kRetryIntervalMs - 1, false) ==
           BleAdmissionAction::kNone);
    assert(updAdv(wrapped, false, rolled + 10 + kRetryIntervalMs, false) ==
           BleAdmissionAction::kStartAdvertising);
    assert(upd(wrapped, false, rolled + 10 + kNoClientTimeoutMs - 1) ==
           BleAdmissionAction::kNone);
    assert(upd(wrapped, false, rolled + 10 + kNoClientTimeoutMs) == BleAdmissionAction::kClose);

    // Event across the rollover grants a full fresh window too.
    BleAdmissionPolicy ev;
    ev.begin(0);
    assert(updEvent(ev, false, UINT32_MAX - 10) == BleAdmissionAction::kNone);
    assert(upd(ev, false, UINT32_MAX - 10 + kNoClientTimeoutMs - 1) ==
           BleAdmissionAction::kNone);
    assert(upd(ev, false, UINT32_MAX - 10 + kNoClientTimeoutMs) == BleAdmissionAction::kClose);
  }

  puts("M7P7B BLE admission policy checks: PASS");
}
