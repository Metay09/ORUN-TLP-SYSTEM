"""6: LoRa ayar komutu — firmware ile ortak vektör ve imzalı cevap."""
import hashlib
import hmac
import struct
import unittest

from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.kdf.hkdf import HKDF

from backend import lora_command as lc

DEV = 0x0E8ADE7E71531AA3
# firmware/tests/m7/test_lora_command.cpp kVectorCommand
VECTOR = ("020700010E8ADE7E71531AA300000001010203040BADC0FFEE12345600000001020100000384030000003C"
          "EDD9ABC7127362652C2CE8D98CDCD396")


def key() -> bytes:
    info = b"ORUN-BLE-WRITE-KEY-v1" + DEV.to_bytes(8, "big") + (1).to_bytes(4, "big")
    return HKDF(hashes.SHA256(), 32, bytes(range(0x10, 0x20)), info).derive(bytes(range(0x40, 0x60)))


class LoraCommandTest(unittest.TestCase):
    def test_shared_vector(self):
        cmd = lc.build_command(key(), DEV, 1, 0x01020304, 0x0BADC0FFEE123456, 1, [(1, 900), (3, 60)])
        self.assertEqual(cmd.hex().upper(), VECTOR)

    def test_refuses_bad_entries(self):
        for entries in ([], [(1, 1)] * 2, [(9, 1)], [(1, 1)] * 9):
            with self.assertRaises(ValueError):
                lc.build_command(key(), DEV, 1, 1, 0, 1, entries)

    def test_result_is_the_trackers_word(self):
        body = struct.pack(">BBBBQIIQIIHBB", 2, 8, 0, 1, DEV, 1, 0x01020304, 0x0BADC0FFEE123456, 2,
                           900, 60, 1, 1)
        res = body + hmac.new(key(), b"ORUN-LORA-RES-v1" + body, hashlib.sha256).digest()[:16]
        r = lc.parse_result(key(), res)
        self.assertEqual((r.outcome, r.command_id, r.revision, r.interval_s, r.ble_window_s),
                         ("APPLIED", 0x01020304, 2, 900, 60))
        with self.assertRaises(lc.BadResult):
            lc.parse_result(key(), res[:-1] + bytes([res[-1] ^ 1]))
        with self.assertRaises(lc.BadResult):
            lc.parse_result(bytes(32), res)


if __name__ == "__main__":
    unittest.main()
