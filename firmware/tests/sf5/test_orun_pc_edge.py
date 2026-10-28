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

    def test_bench_release_command_validates_gateway_id_without_systemd(self):
        script = ROOT / "tools" / "ops" / "orun_pc_edge_service.sh"
        ok = subprocess.run(
            ["bash", str(script), "bench-release", "validate",
             "09A462BD4B275BA5"],
            capture_output=True, text=True, timeout=5, check=False,
        )
        self.assertEqual(ok.returncode, 0, ok.stderr)
        for identifier in ("09a462bd4b275ba5", "09A462BD4B275BA",
                           "09A462BD4B275BA5; touch /tmp/unsafe"):
            bad = subprocess.run(
                ["bash", str(script), "bench-release", "validate", identifier],
                capture_output=True, text=True, timeout=5, check=False,
            )
            self.assertNotEqual(bad.returncode, 0, identifier)

    def test_ble_discovery_uses_ble_device_not_bare_mac(self):
        import asyncio

        class FakeDevice:
            address = "C5:DD:01:85:C9:4B"

        class FakeScanner:
            calls = []
            @classmethod
            async def find_device_by_address(cls, address, timeout=0):
                cls.calls.append(("address", address, timeout))
                return FakeDevice()
            @classmethod
            async def find_device_by_filter(cls, predicate, timeout=0):
                cls.calls.append(("filter", timeout))
                class Ad:
                    service_uuids = [pc.EDGE_SERVICE_UUID.upper()]
                self.assertTrue(predicate(FakeDevice(), Ad()))
                return FakeDevice()

        async def exercise():
            scanner = FakeScanner
            device = await pc.discover_ble_gateway(
                scanner, "C5:DD:01:85:C9:4B")
            self.assertIsInstance(device, FakeDevice)
            self.assertEqual(scanner.calls[-1],
                             ("address", "C5:DD:01:85:C9:4B", 15))
            scanner.calls.clear()
            device = await pc.discover_ble_gateway(scanner, None)
            self.assertIsInstance(device, FakeDevice)
            self.assertEqual(scanner.calls[-1], ("filter", 20))

        asyncio.run(exercise())

    def test_ble_hello_timeout_requires_missing_handshake(self):
        self.assertFalse(pc.ble_hello_expired(10.0, 29.99, False))
        self.assertTrue(pc.ble_hello_expired(10.0, 30.0, False))
        self.assertTrue(pc.ble_hello_expired(10.0, 31.0, False))
        # An otherwise silent but initialized BLE link is normal.
        self.assertFalse(pc.ble_hello_expired(10.0, 999.0, True))

    def test_ble_transport_switch_validation(self):
        script = ROOT / "tools" / "ops" / "orun_pc_edge_service.sh"
        mac = "C5:DD:01:85:C9:4B"
        good = subprocess.run(
            ["bash", str(script), "transport", "validate", "ble", mac],
            capture_output=True, text=True, timeout=5, check=False,
        )
        self.assertEqual(good.returncode, 0, good.stderr)
        for invalid in ("C5:DD:01:85:C9", "C5:DD:01:85:C9:4b",
                        "C5:DD:01:85:C9:4B; reboot"):
            bad = subprocess.run(
                ["bash", str(script), "transport", "validate", "ble", invalid],
                capture_output=True, text=True, timeout=5, check=False,
            )
            self.assertNotEqual(bad.returncode, 0, invalid)

    def test_ble_transport_keeps_backend_token_and_handoff(self):
        import importlib.util
        import shlex

        helper = ROOT / "tools" / "ops" / "orun_pc_edge_transport.py"
        spec = importlib.util.spec_from_file_location("pc_transport_test", helper)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            drop = root / ".config/systemd/user/orun-pc-edge.service.d"
            drop.mkdir(parents=True)
            module.HOME = root
            module.UNIT = drop.parent / "orun-pc-edge.service"
            module.BENCH = drop / "10-bench-release.conf"
            module.UNIT.write_text("[Service]\n")
            token = root / ".config/orun/pc-edge.token"
            port = ("/dev/serial/by-id/"
                    "usb-RAKwireless_WisCore_RAK4631_Board_"
                    "09A462BD4B275BA5-if00")
            cmd = (
                f"/usr/bin/python3 -u {ROOT}/tools/orun_pc_edge.py "
                f"--mode usb --backend-url http://100.114.36.9:8090 "
                f"--edge-id pc-simsek --token-file {token} "
                f"--port {port} --bench-release-gateway 09A462BD4B275BA5"
            )
            module.BENCH.write_text("[Service]\nExecStart=\nExecStart=" + cmd + "\n")
            new = shlex.split(module.build_ble_start("C5:DD:01:85:C9:4B"))
            self.assertEqual(new[:3], ["/usr/bin/python3", "-u",
                                       str(ROOT / "tools/orun_pc_edge.py")])
            self.assertEqual(new[new.index("--mode") + 1], "ble")
            self.assertNotIn("--port", new)
            self.assertEqual(new[new.index("--ble-address") + 1],
                             "C5:DD:01:85:C9:4B")
            self.assertEqual(new[new.index("--token-file") + 1], str(token))
            self.assertEqual(new[new.index("--backend-url") + 1],
                             "http://100.114.36.9:8090")
            self.assertEqual(new[new.index("--bench-release-gateway") + 1],
                             "09A462BD4B275BA5")

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

    def test_bench_accept_requires_successful_backend_and_exact_gateway(self):
        class FakeSerial:
            def __init__(self):
                self.writes = []
            def write(self, data):
                self.writes.append(data)

        with tempfile.TemporaryDirectory() as tmp:
            inbox = pc.Inbox(Path(tmp) / "inbox.sqlite")
            gw = "09A462BD4B275BA5"
            frame = fixture.track(1)[0]
            msg = ("EDGE HELD v1 gw=" + gw +
                   " page=3 slot=2 gen=0000000000000009 len=" +
                   str(len(frame)) + " obj=" + frame.hex().upper())
            self.assertTrue(pc.handle_usb_line(msg, inbox))
            self.assertEqual(inbox.counts()["pending"], 1)
            serial = FakeSerial()
            release = pc.BenchUsbRelease(inbox, gw)
            release.poll(serial, 10.0)
            self.assertEqual(serial.writes, [])  # pending backend: no ACCEPT
            inbox.acknowledge(inbox.batch(), ["UNCERTAIN"])
            release.poll(serial, 14.0)
            self.assertEqual(serial.writes, [])
            inbox.acknowledge(inbox.batch(), ["STORED"])
            pc.BenchUsbRelease(inbox, "AAAAAAAAAAAAAAAA").poll(serial, 18.0)
            self.assertEqual(serial.writes, [])  # gateway-specific authorization
            release.poll(serial, 20.0)
            self.assertEqual(len(serial.writes), 1)
            expected = pc.edge_accept_line(int(gw, 16), 3, 2, 9, frame)
            self.assertEqual(serial.writes[0], (expected + "\n").encode("ascii"))
            release.observe("EDGE ACCEPT OK page=3 slot=2 state=HANDED_OFF")
            self.assertIsNone(inbox.ready_hold(gw))
            with inbox.conn() as db:
                states = db.execute(
                    "SELECT accept_state FROM gateway_holds").fetchall()
            self.assertEqual(states, [("OK",)])

    def test_rejected_backend_never_releases(self):
        class FakeSerial:
            def __init__(self):
                self.writes = []
            def write(self, data):
                self.writes.append(data)

        with tempfile.TemporaryDirectory() as tmp:
            inbox = pc.Inbox(Path(tmp) / "spool.sqlite")
            gw = "0123456789ABCDEF"
            raw = bytes((2, 4, 2)) + bytes(97)
            line = ("EDGE PRIORITY v1 gw=" + gw +
                    " page=1 slot=0 gen=0000000000000001 len=100 obj=" +
                    raw.hex().upper())
            self.assertTrue(pc.handle_usb_line(line, inbox))
            inbox.acknowledge(inbox.batch(), ["REJECTED"])
            fake = FakeSerial()
            release = pc.BenchUsbRelease(inbox, gw)
            release.poll(fake, 12.0)
            self.assertEqual(fake.writes, [])

    def test_bench_accept_timeout_does_not_repeat_forever(self):
        class FakeSerial:
            def __init__(self):
                self.writes = []
            def write(self, data):
                self.writes.append(data)

        with tempfile.TemporaryDirectory() as tmp:
            inbox = pc.Inbox(Path(tmp) / "spool.sqlite")
            gw = "0123456789ABCDEF"
            frame = fixture.track(1)[0]
            self.assertTrue(inbox.ingest(frame, gw))
            inbox.observe_hold(gw, 1, 0, "0000000000000001", frame)
            inbox.acknowledge(inbox.batch(), ["DUPLICATE"])
            fake = FakeSerial()
            release = pc.BenchUsbRelease(inbox, gw)
            for t in (10.0, 14.0, 18.0, 22.0, 26.0):
                release.poll(fake, t)
            self.assertEqual(len(fake.writes), 3)
            self.assertIsNone(inbox.ready_hold(gw))
            with inbox.conn() as db:
                states = db.execute(
                    "SELECT accept_state FROM gateway_holds").fetchall()
            self.assertEqual(states, [("TIMEOUT",)])

    def test_ble_accept_after_backend_commit_three_fragments(self):
        import asyncio
        from orun_edge_bench import edge_accept_binary, EdgeReassembler

        class FakeBleClient:
            def __init__(self):
                self.writes = []
            async def write_gatt_char(self, uuid, frame, response):
                self.writes.append((uuid, bytes(frame), response))

        async def exercise():
            with tempfile.TemporaryDirectory() as tmp:
                inbox = pc.Inbox(Path(tmp) / "spool.sqlite")
                gw = "09A462BD4B275BA5"
                raw = fixture.track(1)[0]
                hello = bytes([1]) + bytes.fromhex(gw) + bytes(4)
                connected = pc.handle_ble_message(hello, inbox, None)
                self.assertEqual(connected, gw)
                held = (
                    bytes([2]) + (5).to_bytes(2, "big") +
                    (7).to_bytes(2, "big") + (99).to_bytes(8, "big") +
                    bytes(4) + bytes([len(raw)]) + raw
                )
                self.assertEqual(pc.handle_ble_message(held, inbox, gw), gw)
                fake = FakeBleClient()
                release = pc.BenchBleRelease(inbox, gw)
                self.assertEqual(await release.poll_ble(fake, 10, 0), 0)
                self.assertEqual(fake.writes, [])
                inbox.acknowledge(inbox.batch(), ["STORED"])
                self.assertEqual(await release.poll_ble(fake, 12, 0), 1)
                self.assertEqual(len(fake.writes), 3)
                self.assertTrue(all(row[0] == pc.EDGE_TO_GATEWAY_UUID and
                                    row[2] is True and len(row[1]) <= 20
                                    for row in fake.writes))
                rx = EdgeReassembler()
                messages = [rx.push(row[1]) for row in fake.writes]
                self.assertEqual(
                    messages[-1],
                    edge_accept_binary(int(gw, 16), 5, 7, 99, raw))
                # Wrong page/slot must never acknowledge an in-flight handoff.
                release.observe_message(
                    bytes([4]) + (5).to_bytes(2, "big") +
                    (8).to_bytes(2, "big") + bytes([0]))
                self.assertIsNotNone(inbox.ready_hold(gw))
                # code 0 = confirmed Gateway handoff
                release.observe_message(
                    bytes([4]) + (5).to_bytes(2, "big") +
                    (7).to_bytes(2, "big") + bytes([0]))
                self.assertIsNone(inbox.ready_hold(gw))
                with inbox.conn() as db:
                    self.assertEqual(db.execute(
                        "SELECT accept_state FROM gateway_holds").fetchone(),
                        ("OK",))
        asyncio.run(exercise())

    def test_ble_rejected_backend_no_accept(self):
        import asyncio

        class FakeBleClient:
            def __init__(self):
                self.writes = []
            async def write_gatt_char(self, uuid, frame, response):
                self.writes.append((uuid, bytes(frame), response))

        async def exercise():
            with tempfile.TemporaryDirectory() as tmp:
                inbox = pc.Inbox(Path(tmp) / "spool.sqlite")
                gw = "0123456789ABCDEF"
                raw = bytes((2, 4, 2)) + bytes(97)
                inbox.ingest(raw, gw)
                inbox.observe_hold(gw, 4, 0, "0000000000000001", raw)
                inbox.acknowledge(inbox.batch(), ["REJECTED"])
                fake = FakeBleClient()
                release = pc.BenchBleRelease(inbox, gw)
                self.assertEqual(await release.poll_ble(fake, 10, 0), 0)
                self.assertEqual(fake.writes, [])
        asyncio.run(exercise())

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
