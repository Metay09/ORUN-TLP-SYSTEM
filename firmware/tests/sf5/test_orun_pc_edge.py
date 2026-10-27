#!/usr/bin/env python3
"""Host-only PC Edge sealed spool/backoff tests; never touch real credentials."""
from __future__ import annotations

from pathlib import Path
import sqlite3
import subprocess
import stat
import sys
import tempfile
import threading
import unittest
from urllib.error import URLError

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "backend"))
sys.path.insert(0, str(Path(__file__).parent))
import orun_pc_edge as pc
import orun_backend_bench as backend
import test_sf5l3_edge_map as fixture

TOKEN = "offline-test-only-secret"


class PCEdgeTests(unittest.TestCase):
    def token(self, root: Path) -> Path:
        path = root / "token"
        path.write_text(TOKEN + "\n", encoding="utf8")
        path.chmod(0o600)
        return path

    def test_installer_accepts_real_rak_usb_by_id(self):
        script = ROOT / "tools" / "ops" / "orun_pc_edge_service.sh"
        real_port = ("/dev/serial/by-id/"
                     "usb-RAKwireless_WisCore_RAK4631_Board_"
                     "09A462BD4B275BA5-if00")
        valid = subprocess.run(
            ["bash", str(script), "validate", "usb",
             "http://100.114.36.9:8090", "pc-simsek", real_port],
            capture_output=True, text=True, timeout=5, check=False,
        )
        self.assertEqual(valid.returncode, 0, valid.stderr)
        invalid = subprocess.run(
            ["bash", str(script), "validate", "usb",
             "http://100.114.36.9:8090", "pc-simsek", "/dev/ttyUSB0"],
            capture_output=True, text=True, timeout=5, check=False,
        )
        self.assertNotEqual(invalid.returncode, 0)
        self.assertIn("USB portu", invalid.stderr)

    def test_url_and_token_guard(self):
        assert pc.checked_backend("http://100.99.88.77:8090") == "http://100.99.88.77:8090"
        assert pc.checked_backend("http://127.0.0.1:8090") == "http://127.0.0.1:8090"
        for url in ("http://8.8.8.8:8090", "http://0.0.0.0:8090",
                    "https://100.100.100.100:8090", "http://100.100.100.100:8000",
                    "http://x:password@100.100.100.100:8090"):
            with self.assertRaises(ValueError, msg=url):
                pc.checked_backend(url)
        with tempfile.TemporaryDirectory() as folder:
            path = self.token(Path(folder))
            self.assertEqual(pc.read_token(path), TOKEN)
            path.chmod(0o644)
            with self.assertRaises(ValueError):
                pc.read_token(path)

    def test_usb_ble_spool_duplicate_and_restart(self):
        with tempfile.TemporaryDirectory() as folder:
            db = Path(folder) / "spool.sqlite"
            inbox = pc.Inbox(db)
            raw = bytes((2, 4, 2)) + bytes(97)  # shape-valid, authentication pending
            self.assertTrue(inbox.ingest(raw, "0123456789ABCDEF"))
            line = ("EDGE PRIORITY v1 gw=0123456789ABCDEF page=1 slot=0 "
                    "gen=0123456789ABCDEF len=100 obj=" + raw.hex().upper())
            self.assertTrue(pc.handle_usb_line(line, inbox))
            self.assertFalse(pc.handle_usb_line(line[:-8], inbox))
            hello = bytes([1]) + bytes.fromhex("0123456789ABCDEF") + bytes(4)
            gw = pc.handle_ble_message(hello, inbox, None)
            self.assertEqual(gw, "0123456789ABCDEF")
            held = (bytes([6]) + bytes(2 + 2 + 8 + 1 + 2 + 1) + bytes([100]) + raw)
            self.assertEqual(pc.handle_ble_message(held, inbox, gw), gw)
            self.assertEqual(inbox.counts()["pending"], 1)
            self.assertEqual(pc.Inbox(db).counts()["pending"], 1)
            inbox.acknowledge(inbox.batch(), ["UNCERTAIN"])
            self.assertEqual(inbox.counts()["pending"], 1)
            with self.assertRaises(ValueError):
                inbox.acknowledge(inbox.batch(), ["UNKNOWN"])
            inbox.acknowledge(inbox.batch(), ["REJECTED"])
            self.assertEqual(inbox.counts()["quarantined"], 1)
            self.assertEqual(inbox.counts()["pending"], 0)
            with sqlite3.connect(db) as conn:
                self.assertEqual(conn.execute(
                    "SELECT COUNT(*) FROM sealed_records").fetchone()[0], 1)

    def test_backend_roundtrip_and_retry(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            inbox = pc.Inbox(root / "edge.sqlite")
            frame = fixture.track(1)[0]  # authentic SF5L0 public *test* vector
            self.assertTrue(inbox.ingest(frame, "09A462BD4B275BA5"))
            token = self.token(root)
            uploader = pc.Uploader(inbox, "http://127.0.0.1:8090", token, "pc-test")
            uploader.base = "http://127.0.0.1:1"  # definitely not this backend
            with self.assertRaises((OSError, URLError)):
                uploader.once()
            self.assertEqual(inbox.counts()["pending"], 1)

            app = backend.Backend(root / "server.sqlite", True)
            server = backend.make_server(app, "127.0.0.1", 0, TOKEN)
            worker = threading.Thread(target=server.serve_forever, daemon=True)
            worker.start()
            try:
                uploader.base = "http://127.0.0.1:" + str(server.server_address[1])
                self.assertEqual(uploader.once(), 1)
                self.assertEqual(inbox.counts()["delivered"], 1)
                self.assertEqual(inbox.counts()["pending"], 0)
                self.assertEqual(app.ledger.record_count(), 1)
                self.assertEqual(uploader.once(), 0)
            finally:
                server.shutdown()
                server.server_close()
                worker.join(timeout=2)


if __name__ == "__main__":
    unittest.main()
