"""Alan belleği durumu (tools/orun_cihaz_kontrol.py alan): yalnız artık varken hazırlanır."""
from __future__ import annotations

import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location(
    "orun_cihaz_kontrol", Path(__file__).resolve().parents[2] / "tools" / "orun_cihaz_kontrol.py")
k = importlib.util.module_from_spec(spec)
spec.loader.exec_module(k)


def diag(a: str, a_tail: str, b: str, b_tail: str, maintenance: str) -> list[str]:
    return [
        "APP FENCE DIAG read_only=yes scope=0x0E5000..0x0E6FFF",
        f"APP FENCE DIAG page=A classified=yes evidence={a} all_ff=no tail_ff={a_tail} crc32=1 first_non_ff=0",
        f"APP FENCE DIAG page=B classified=yes evidence={b} all_ff=no tail_ff={b_tail} crc32=2 first_non_ff=0",
        f"APP FENCE DIAG maintenance={maintenance}",
    ]


class AlanBellegi(unittest.TestCase):
    def state(self, *args: str) -> str:
        return k.fence_verdict(k.parse_fence(diag(*args)))[0]

    def test_field_signature_is_leftover(self):
        # 52E16773141A8887, 9 Kasım 2026 (onarımdan önce).
        self.assertEqual(self.state("SUPPORTED_CORRUPT", "no", "ERASED", "yes", "yes"), "kalinti")

    def test_healthy_and_blank(self):
        self.assertEqual(self.state("COMMITTED_CLEAR", "yes", "ERASED", "yes", "no"), "hazir")
        self.assertEqual(self.state("COMMITTED_CONFIGURED", "yes", "COMMITTED_CLEAR", "yes", "no"), "hazir")
        self.assertEqual(self.state("ERASED", "yes", "ERASED", "yes", "no"), "bos")

    def test_never_preparable_when_an_area_record_exists(self):
        self.assertEqual(self.state("COMMITTED_CONFIGURED", "yes", "SUPPORTED_CORRUPT", "no", "yes"), "bakim")
        self.assertEqual(self.state("COMMITTED_CLEAR", "no", "ERASED", "yes", "yes"), "bakim")
        self.assertEqual(self.state("UNSUPPORTED_NEWER", "yes", "ERASED", "yes", "yes"), "bakim")

    def test_no_answer_is_unknown(self):
        self.assertEqual(k.fence_verdict(k.parse_fence(["APP DEVICE id=1"]))[0], "bilinmiyor")


if __name__ == "__main__":
    unittest.main()
