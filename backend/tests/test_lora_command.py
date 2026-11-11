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


VECTOR_AREAS = ("020700020E8ADE7E71531AA3000000010A0B0C0D0102030405060708000000010001010003010316694E0010B81720166AD4A010B8172016694E0010B99DC06673E256DA8890748DFEF81325A81C72")


class LoraCommandTest(unittest.TestCase):
    def test_area_vector(self):
        frames = lc.build_area_command(key(), DEV, 1, 0x0A0B0C0D, 0x0102030405060708, 1,
                                       [[(376000000, 280500000), (376100000, 280500000), (376000000, 280600000)]])
        self.assertEqual([f.hex().upper() for f in frames], [VECTOR_AREAS])
        big = [[(376000000 + a * 100000 + i, 280500000 + i) for i in range(8)] for a in range(8)]
        frames = lc.build_area_command(key(), DEV, 1, 1, 0, 0, big)
        self.assertEqual([len(f) for f in frames], [255, 255, 170])
        self.assertEqual(len(lc.build_area_command(key(), DEV, 1, 1, 0, 0, [])[0]), 54)
        with self.assertRaises(ValueError):
            lc.area_set([[(0, 0)]] * 9)

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
