#!/usr/bin/env python3
"""KQ-05 host-only delivery model. NOT an RF, flash, or crypto implementation."""
import sys
import unittest
from dataclasses import dataclass, field

MAX_IDS = 6  # Existing HistorySecure BACKEND_DURABLE receipt
RF_HOUR_MS = 360000


@dataclass(frozen=True)
class Record:
    device: int
    incarnation: int
    sequence: int


@dataclass(frozen=True)
class Receipt:
    device: int
    incarnation: int
    ids: tuple


@dataclass
class Collar:
    device: int
    incarnation: int
    capacity: int
    next_seq: int = 101
    retained: dict = field(default_factory=dict)
    sent_live: list = field(default_factory=list)
    flash_errors: int = 0

    def append(self):
        if len(self.retained) >= self.capacity:
            self.flash_errors += 1
            raise OverflowError("FLASH_FULL: unconfirmed records cannot disappear")
        record = Record(self.device, self.incarnation, self.next_seq)
        self.next_seq += 1
        self.retained[record.sequence] = record
        return record

    def send_live(self, record):
        if self.retained.get(record.sequence) != record:
            raise ValueError("store-first requirement")
        self.sent_live.append(record.sequence)  # No per-record ACK gate.

    def apply_receipt(self, receipt, *, authenticated=False):
        # A test-injected verdict, not a cryptographic authenticator.
        # Production must use HistorySecureCrypto + replay owner first.
        ids = receipt.ids
        if (not authenticated or receipt.device != self.device or
            receipt.incarnation != self.incarnation or
            not 1 <= len(ids) <= MAX_IDS or
            tuple(sorted(set(ids))) != ids or
            any(seq not in self.retained for seq in ids)):
            return 0
        for seq in ids:
            del self.retained[seq]
        return len(ids)


@dataclass
class Gateway:
    durable: dict = field(default_factory=dict)

    def store(self, record):
        key = record.device, record.incarnation
        self.durable.setdefault(key, {})[record.sequence] = record
        assert self.durable[key][record.sequence] == record

    def receipts(self, device, incarnation, ids):
        selected = tuple(sorted(set(ids)))
        proven = self.durable.get((device, incarnation), {})
        if any(seq not in proven for seq in selected):
            raise ValueError("no durable proof for claimed record")
        return [Receipt(device, incarnation, selected[i:i + MAX_IDS])
                for i in range(0, len(selected), MAX_IDS)]


def airtime_ms(n):
    """KQ-04 SF11/BW125/4:5/8-preamble/CRC/explicit LoRa airtime."""
    if not 1 <= n <= 255:
        raise ValueError("invalid payload")
    de = 1
    numerator = 8 * n - 4 * 11 + 28 + 16
    denominator = 4 * (11 - 2 * de)
    blocks = max(0, (numerator + denominator - 1) // denominator)
    quarters = 4 * 8 + 17 + 4 * (8 + blocks * 5)
    return (quarters * 2048 * 1000 + 4 * 125000 - 1) // (4 * 125000)


def hourly_comparison(collars, report_min, receipt_min=5):
    """No loss/retry; receipt length from EXISTING backend HistorySecure format."""
    if 60 % report_min or 60 % receipt_min:
        raise ValueError("cadence must divide 60")
    single = collars * (60 // report_min) * airtime_ms(56)
    pending, batch_ms, batch_count = 0, 0, 0
    for minute in range(60):
        if minute % report_min == 0:
            pending += 1
        if (minute + 1) % receipt_min == 0:
            while pending:
                take = min(pending, MAX_IDS)
                # HistorySecure 36B header + 4B fixed + 8B/id + 8B tag.
                batch_ms += airtime_ms(48 + 8 * take)
                batch_count += 1
                pending -= take
    return single, collars * batch_ms, collars * batch_count


class Kq05Tests(unittest.TestCase):
    def test_sf11_lengths(self):
        self.assertEqual(tuple(airtime_ms(n) for n in (56, 84, 88, 100)),
                         (1397, 1889, 1971, 2216))

    def test_gap_kept_and_new_live_sent(self):
        collar, gateway = Collar(12, 34, 20), Gateway()
        for _ in range(5):
            record = collar.append()
            collar.send_live(record)
            if record.sequence != 103:
                gateway.store(record)
        a = gateway.receipts(12, 34, (101, 102, 104, 105))
        self.assertEqual(collar.apply_receipt(a[0], authenticated=True), 4)
        self.assertEqual(set(collar.retained), {103})
        collar.send_live(collar.append())
        self.assertEqual(collar.sent_live[-1], 106)
        self.assertEqual(set(collar.retained), {103, 106})
        with self.assertRaises(ValueError):
            gateway.receipts(12, 34, (103,))

    def test_wrong_authority_and_identity(self):
        c = Collar(12, 34, 10)
        c.append()
        for r in (Receipt(13, 34, (101,)), Receipt(12, 35, (101,)),
                  Receipt(12, 34, (101, 101)), Receipt(12, 34, (101, 102))):
            self.assertEqual(c.apply_receipt(r, authenticated=True), 0)
        good = Receipt(12, 34, (101,))
        self.assertEqual(c.apply_receipt(good, authenticated=False), 0)
        self.assertEqual(c.apply_receipt(good, authenticated=True), 1)
        self.assertEqual(c.apply_receipt(good, authenticated=True), 0)

    def test_six_explicit_ids_per_receipt(self):
        c, g = Collar(12, 34, 30), Gateway()
        for _ in range(20):
            g.store(c.append())
        receipts = g.receipts(12, 34, range(101, 121))
        self.assertEqual([len(r.ids) for r in receipts], [6, 6, 6, 2])

    def test_48h_without_ack_live_is_not_cache_gated(self):
        c = Collar(12, 34, 3000)  # Model capacity, NOT RAK real capacity.
        for _ in range(2880):
            c.send_live(c.append())
        self.assertEqual((len(c.retained), len(c.sent_live)), (2880, 2880))

    def test_flash_full_is_not_silent(self):
        c = Collar(12, 34, 4)
        for _ in range(4):
            c.send_live(c.append())
        with self.assertRaisesRegex(OverflowError, "FLASH_FULL"):
            c.append()
        self.assertEqual(c.flash_errors, 1)

    def test_four_one_minute_load(self):
        self.assertEqual(hourly_comparison(4, 1), (335280, 94608, 48))
        self.assertLess(hourly_comparison(4, 1)[1], RF_HOUR_MS)

    def test_hourly_reporting_single_ack_cheap(self):
        self.assertEqual(hourly_comparison(4, 60), (5588, 5588, 4))


def print_table():
    print("KQ-05 HOST MODEL ONLY: no RF, crypto, flash, loss or retransmission")
    print("4 collars: interval, one-by-one ACK ms/h, 5min receipt ms/h, frames")
    for interval in (1, 2, 5, 15, 60):
        print(interval, *hourly_comparison(4, interval))


if __name__ == "__main__":
    if "--test" in sys.argv:
        unittest.main(argv=[sys.argv[0]], verbosity=2)
    else:
        print_table()
