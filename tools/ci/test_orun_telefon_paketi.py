"""python3 -m unittest discover -s tools/ci -p 'test_orun_telefon_paketi.py'"""
from __future__ import annotations

import importlib.util
import json
import tempfile
import unittest
from pathlib import Path
from zipfile import ZipFile

MODULE = Path(__file__).resolve().parents[1] / "orun_telefon_paketi.py"
spec = importlib.util.spec_from_file_location("orun_telefon_paketi", MODULE)
assert spec and spec.loader
pkg = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pkg)
SHA = "0123456789abcdef0123456789abcdef01234567"


class PhonePackageTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name)
        self.src = self.path / "dfu.zip"
        self.dest = self.path / "orun.zip"

    def make_dfu(self, section=None, extra=None):
        if section is None:
            section = {"application": {"bin_file": "firmware.bin", "dat_file": "firmware.dat"}}
        with ZipFile(self.src, "w") as z:
            z.writestr("manifest.json", json.dumps({"manifest": section}))
            z.writestr("firmware.bin", b"\x01\x02\x03")
            z.writestr("firmware.dat", b"\x04\x05")
            if extra:
                z.writestr(extra, b"unexpected")

    def test_application_package_preserves_bytes_and_hashes(self):
        self.make_dfu()
        meta = pkg.package_dfu(self.src, self.dest, SHA)
        self.assertEqual(meta["target"]["board"], "RAK4630")
        self.assertEqual(meta["source_commit"], SHA)
        with ZipFile(self.dest) as z:
            self.assertEqual(set(z.namelist()), {"orun-package.json", "application.bin", "application.dat"})
            self.assertEqual(z.read("application.bin"), b"\x01\x02\x03")
            self.assertEqual(z.read("application.dat"), b"\x04\x05")
            self.assertEqual(json.loads(z.read("orun-package.json")), meta)
            self.assertEqual(meta["application"]["bin_sha256"], pkg._sha(z.read("application.bin")))
        with self.assertRaisesRegex(pkg.PackageError, "Çıktı zaten var"):
            pkg.package_dfu(self.src, self.dest, SHA)

    def test_refuses_non_application_flash_images(self):
        self.make_dfu({"application": {"bin_file": "firmware.bin", "dat_file": "firmware.dat"},
                       "bootloader": {"bin_file": "bootloader.bin"}})
        with self.assertRaisesRegex(pkg.PackageError, "bootloader/SoftDevice"):
            pkg.package_dfu(self.src, self.dest, SHA)
        self.assertFalse(self.dest.exists())

    def test_refuses_extra_path_and_tampering(self):
        self.make_dfu(extra="../escape.txt")
        with self.assertRaisesRegex(pkg.PackageError, "güvensiz"):
            pkg.package_dfu(self.src, self.dest, SHA)
        self.make_dfu(extra="malware.sh")
        with self.assertRaisesRegex(pkg.PackageError, "beklenmeyen"):
            pkg.package_dfu(self.src, self.dest, SHA)
        self.src.write_bytes(b"not a zip")
        with self.assertRaises(pkg.PackageError):
            pkg.package_dfu(self.src, self.dest, SHA)
        self.assertFalse(self.dest.exists())

    def test_refuses_wrong_commit_and_missing_image(self):
        self.make_dfu({"application": {"bin_file": "missing.bin", "dat_file": "firmware.dat"}})
        with self.assertRaises(pkg.PackageError):
            pkg.package_dfu(self.src, self.dest, SHA)
        with self.assertRaisesRegex(pkg.PackageError, "40 haneli"):
            pkg.package_dfu(self.src, self.dest, "abc")


if __name__ == "__main__":
    unittest.main()
