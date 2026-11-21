"""Safety regression for the existing tools/orun_guncelle.sh entrypoint.

These tests never contact GitHub, open a database, or touch systemd services.
"""
from __future__ import annotations

import re
import subprocess
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools" / "orun_guncelle.sh"


class SafeUpdaterTests(unittest.TestCase):
    def test_bash_syntax(self) -> None:
        subprocess.run(["bash", "-n", str(SCRIPT)], check=True)

    def test_help_needs_no_machine_access(self) -> None:
        done = subprocess.run(
            ["bash", str(SCRIPT), "--help"],
            capture_output=True, text=True, check=True, timeout=5,
        )
        self.assertIn("--check", done.stdout)
        self.assertIn("--server", done.stdout)
        self.assertIn("--apk", done.stdout)

    def test_all_embedded_python_parses(self) -> None:
        source = SCRIPT.read_text()
        blocks = re.findall(
            r"(?m)^[^\n]*python3[^\n]*<<'PY'\n([\s\S]*?)^PY$",
            source,
        )
        self.assertEqual(len(blocks), 5)
        for i, block in enumerate(blocks):
            with self.subTest(block=i):
                compile(block, f"orun_guncelle.sh:python:{i}", "exec")

    def test_no_implicit_firmware_or_apk(self) -> None:
        text = SCRIPT.read_text()
        self.assertIn('BUILD_APK=0', text)
        self.assertIn('[[ "$BUILD_APK" == 1 ]] || exit 0', text)
        self.assertNotIn("orun_cihaz_yukle.sh", text)
        self.assertNotIn("pio run --target upload", text)
        self.assertIn("UPDATE_EDGE=0", text)
        self.assertIn("backup_live", text)
        self.assertIn("migrate_latest_indexes", text)


if __name__ == "__main__":
    unittest.main()
