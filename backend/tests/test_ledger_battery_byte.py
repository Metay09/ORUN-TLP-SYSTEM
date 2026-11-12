"""Pil 2: the server accepts the bit-packed battery byte (wire spec 7.4)."""
from __future__ import annotations

import importlib.util
from pathlib import Path
import sys
import unittest

spec = importlib.util.spec_from_file_location(
    "product_secure_verified_ledger",
    Path(__file__).resolve().parents[1] / "candidates" / "product_secure_verified_ledger.py")
led = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = led  # dataclasses look the module up
spec.loader.exec_module(led)


def periodic(battery_byte: int, battery_valid: bool = True) -> bytes:
    p = bytearray(64)
    p[0] = 1
    p[1] = 32 if battery_valid else 0
    p[3] = 1 if battery_valid else 0
    p[4:12] = (1).to_bytes(8, "big")
    p[12:16] = (1).to_bytes(4, "big")
    p[20:24] = (300).to_bytes(4, "big")
    p[38:40] = (65535).to_bytes(2, "big")
    p[42] = battery_byte
    if battery_valid:
        p[56:58] = (4100).to_bytes(2, "big")
    return bytes(p)


class BatteryByteTest(unittest.TestCase):
    def test_packed_values(self):
        self.assertTrue(led._periodic_valid(periodic(1)))                       # before Pil 2
        self.assertTrue(led._periodic_valid(periodic(1 | 3 << 2 | 0x20)))       # FULL, USB
        self.assertTrue(led._periodic_valid(periodic(1 | 4 << 2 | 0x20)))       # NO_BATTERY
        self.assertFalse(led._periodic_valid(periodic(3)))                      # quality 3
        self.assertFalse(led._periodic_valid(periodic(1 | 5 << 2)))             # power 5
        self.assertFalse(led._periodic_valid(periodic(1 | 0x40)))               # reserved
        self.assertTrue(led._periodic_valid(periodic(0, battery_valid=False)))
        self.assertFalse(led._periodic_valid(periodic(1 << 2, battery_valid=False)))


if __name__ == "__main__":
    unittest.main()
