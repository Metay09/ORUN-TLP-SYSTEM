#!/usr/bin/env python3
"""KQ-04 PC-only regulatory-budget and capacity risk contract.

This is an executable *reference model*, NOT the firmware enforcement path.
A PASS does not certify actual RF output or make the existing 6%-product-only
pacer into a device-wide 10% duty-cycle guard. No hardware/network/db access.
"""

from collections import deque
import unittest


WINDOW_MS = 60 * 60 * 1000
MAX_AIRTIME_MS = WINDOW_MS // 10     # 10% device-wide limit per rolling hour
PAGE_RECORDS = 42
DATA_PAGES = 30
DEVICE_RECORD_CAPACITY = PAGE_RECORDS * DATA_PAGES

FRAME_CLASSES = (
    "PRODUCT_SECURE", "POSITION", "HISTORY_SECURE", "RELAY",
    "CUSTODY_ACK", "LORA_COMMAND_RESULT", "TEST",
)


class RollingDutyReference:
    """Fail-closed, monotonic host reference for *all* frame classes.

    The production owner must also persist/account for TX across resets and
    calculate actual on-air time from LoRa params, not this model's estimates.
    """

    def __init__(self):
        self.sent = deque()
        self.total = 0
        self.last_now = None
        self.per_class = {kind: 0 for kind in FRAME_CLASSES}

    def try_send(self, now_ms, airtime_ms, kind):
        if kind not in self.per_class:
            raise ValueError("unregistered radio frame class")
        if (not isinstance(now_ms, int) or not isinstance(airtime_ms, int)
                or airtime_ms <= 0 or airtime_ms > MAX_AIRTIME_MS):
            raise ValueError("invalid timestamp or airtime")
        if self.last_now is not None and now_ms < self.last_now:
            raise ValueError("monotonic time went backwards")
        self.last_now = now_ms
        while self.sent and self.sent[0][0] <= now_ms - WINDOW_MS:
            _time, duration, _kind = self.sent.popleft()
            self.total -= duration
        if self.total + airtime_ms > MAX_AIRTIME_MS:
            return False
        self.sent.append((now_ms, airtime_ms, kind))
        self.total += airtime_ms
        self.per_class[kind] += 1
        return True


class TestKQ04Budget(unittest.TestCase):
    def test_all_packet_classes_share_one_limit(self):
        budget = RollingDutyReference()
        for i, kind in enumerate(FRAME_CLASSES):
            self.assertTrue(budget.try_send(i * 1000, 45_000, kind))
        self.assertEqual(budget.total, 315_000)
        self.assertFalse(budget.try_send(7000, 46_000, "PRODUCT_SECURE"))
        self.assertTrue(budget.try_send(8000, 45_000, "CUSTODY_ACK"))
        self.assertEqual(budget.total, MAX_AIRTIME_MS)
        self.assertFalse(budget.try_send(9000, 1, "POSITION"))

    def test_rolling_hour_expires_old_airtime(self):
        budget = RollingDutyReference()
        self.assertTrue(budget.try_send(0, MAX_AIRTIME_MS, "PRODUCT_SECURE"))
        self.assertFalse(budget.try_send(WINDOW_MS - 1, 1, "POSITION"))
        self.assertTrue(budget.try_send(WINDOW_MS, 1, "POSITION"))

    def test_retries_and_acks_both_consume_budget(self):
        budget = RollingDutyReference()
        sent = 0
        blocked = 0
        for minute in range(60):
            for kind, toa in (("PRODUCT_SECURE", 2300),
                              ("CUSTODY_ACK", 1100),
                              ("POSITION", 1300),
                              ("HISTORY_SECURE", 2300)):
                if budget.try_send(minute * 60000, toa, kind):
                    sent += toa
                else:
                    blocked += 1
        self.assertGreater(blocked, 0)
        self.assertLessEqual(sent, MAX_AIRTIME_MS)
        self.assertEqual(sent, budget.total)

    def test_reject_unknown_classes_and_bad_timing(self):
        budget = RollingDutyReference()
        with self.assertRaises(ValueError):
            budget.try_send(0, 2000, "UNACCOUNTED_TX")
        with self.assertRaises(ValueError):
            budget.try_send(0, 0, "POSITION")
        budget.try_send(100, 100, "POSITION")
        with self.assertRaises(ValueError):
            budget.try_send(99, 100, "POSITION")

    def test_no_grant_for_airtime_over_single_hour_limit(self):
        budget = RollingDutyReference()
        with self.assertRaises(ValueError):
            budget.try_send(0, MAX_AIRTIME_MS + 1, "TEST")

    def test_capacity_24h_outage_exceeds_one_minute_record_space(self):
        # Upper-bound physical geometry, not exact flash rotation behavior.
        produced = 24 * 60
        self.assertEqual(DEVICE_RECORD_CAPACITY, 1260)
        self.assertEqual(produced - DEVICE_RECORD_CAPACITY, 180)

    def test_capacity_48h_outage_exceeds_flash_space(self):
        self.assertEqual(48 * 60 - DEVICE_RECORD_CAPACITY, 1620)

    def test_six_percent_is_not_aggregate_ten_percent(self):
        # Existing bench pacer accounts ONLY for PRODUCT_SECURE traffic.
        product_budget_ms = WINDOW_MS * 6 // 100
        self.assertEqual(product_budget_ms, 216_000)
        # Other traffic can push the device over 10% if unaccounted.
        nonproduct_ms = 150_000
        self.assertGreater(product_budget_ms + nonproduct_ms,
                           MAX_AIRTIME_MS)


if __name__ == "__main__":
    unittest.main()
