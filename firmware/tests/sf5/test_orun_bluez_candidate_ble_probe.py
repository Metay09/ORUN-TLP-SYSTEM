#!/usr/bin/env python3
"""Offline regression: read-only BLE probe must decode HELLO + DUMP_END.

Stubs Bleak; no Bluetooth adapter, systemd service or Gateway is accessed.
"""
import asyncio
import contextlib
import importlib.util
import io
from pathlib import Path
import sys
import types
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools"))
from orun_edge_bench import edge_frames, EDGE_TO_EDGE_UUID, EDGE_TO_GATEWAY_UUID

# The probe imports Bleak at module import time, but physical BLE is not
# required for this test. No system package installation required.
fake_bleak = types.ModuleType("bleak")
fake_bleak.BleakClient = object
fake_bleak.BleakScanner = object
old_bleak = sys.modules.get("bleak")
sys.modules["bleak"] = fake_bleak
try:
    spec = importlib.util.spec_from_file_location(
        "orun_bluez_candidate_ble_probe",
        ROOT / "tools" / "ops" / "orun_bluez_candidate_ble_probe.py")
    probe = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(probe)
finally:
    if old_bleak is None:
        del sys.modules["bleak"]
    else:
        sys.modules["bleak"] = old_bleak


class CandidateNotificationTests(unittest.TestCase):
    def test_indication_callback_receives_hello_and_dump_without_ack(self):
        instances = []
        gateway = bytes.fromhex("09A462BD4B275BA5")
        messages = [b"\x01" + gateway + bytes((0, 0, 0, 6)),
                    bytes((3, 0, 0))]

        class Characteristic:
            def __init__(self, properties):
                self.properties = properties

        class Services:
            def get_characteristic(self, uuid):
                return Characteristic(
                    ["indicate"] if uuid.lower() == EDGE_TO_EDGE_UUID else ["write"])

        class Client:
            is_connected = True
            services = Services()

            def __init__(self, dev, timeout):
                self.writes = []
                self.callback = None
                instances.append(self)

            async def __aenter__(self):
                return self

            async def __aexit__(self, *_):
                return False

            async def start_notify(self, uuid, callback):
                assert uuid == EDGE_TO_EDGE_UUID
                self.callback = callback

            async def write_gatt_char(self, uuid, payload, response):
                self.writes.append((uuid, bytes(payload), response))
                if bytes(payload) == bytes((1, 3, 1, 0, 0x81)):
                    for mid, message in enumerate(messages, start=2):
                        for frame in edge_frames(message, mid):
                            self.callback(Characteristic(["indicate"]), bytearray(frame))

        async def discover(_, address):
            self.assertEqual(address, "C5:DD:01:85:C9:4B")
            return object()

        previous = (probe.BleakClient, probe.discover_ble_gateway)
        probe.BleakClient = Client
        probe.discover_ble_gateway = discover
        try:
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                rc = asyncio.run(probe.run())
        finally:
            probe.BleakClient, probe.discover_ble_gateway = previous

        self.assertEqual(rc, 0, output.getvalue())
        self.assertIn("MATCH: True", output.getvalue())
        self.assertIn("DUMP END: 0", output.getvalue())
        self.assertIn("RESULT: PASS", output.getvalue())
        self.assertEqual(len(instances), 1)
        self.assertEqual(instances[0].writes,
                         [(EDGE_TO_GATEWAY_UUID,
                           bytes((1, 3, 1, 0, 0x81)), True)])


if __name__ == "__main__":
    unittest.main()
