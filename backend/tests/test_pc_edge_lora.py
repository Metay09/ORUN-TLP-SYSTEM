"""6c: PC Edge LoRa köprüsü — komutu gateway'e verir, gateway'in dediğini bildirir."""
from __future__ import annotations

import io
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import orun_pc_edge as pe  # noqa: E402

DEV = "52E16773141A8887"
HEX = "AB" * 54


class FakeOpener:
    def __init__(self, commands):
        self.commands, self.posted = commands, []

    def open(self, req, timeout=0):
        if req.get_method() == "POST":
            self.posted.append(json.loads(req.data))
            body = {"ok": True}
        else:
            body = {"commands": self.commands}
        return io.BytesIO(json.dumps(body).encode())


class FakeSerial:
    def __init__(self):
        self.written = []

    def write(self, data):
        self.written.append(data.decode())


class LoraBridgeTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.token = Path(tmp.name) / "token"
        self.token.write_text("t" * 32)
        self.token.chmod(0o600)

    def test_hands_once_and_reports_what_the_gateway_said(self):
        opener = FakeOpener([{"device": DEV, "command_id": 7, "hex": HEX}])
        lora, ser = pe.LoraBridge("http://x", self.token, opener), FakeSerial()
        lora.poll(ser, 100.0)
        self.assertEqual(ser.written, [f"APP LORA QUEUE {HEX}\n"])
        lora.poll(ser, 130.0)               # same command: not handed again
        self.assertEqual(len(ser.written), 1)
        lora.observe("LORA QUEUED result=ADDED waiting=1")
        lora.observe(f"LORA SENT target={DEV} id=00000007 attempt=1/6")
        lora.observe("LORA RESULT queued=yes rssi=-40 hex=" + "CD" * 56)
        lora.poll(ser, 131.0)
        kinds = [e["kind"] for e in opener.posted[0]["events"]]
        self.assertEqual(kinds, ["queued", "sent", "result"])
        self.assertEqual(opener.posted[0]["events"][0], {"kind": "queued", "device": DEV, "command_id": 7})
        # Dropped by the gateway: handed again at the next check.
        lora.observe(f"LORA DROPPED target={DEV} id=00000007")
        lora.poll(ser, 200.0)
        self.assertEqual(len(ser.written), 2)
        # A reconnected gateway holds nothing: handed again.
        lora.reset()
        lora.poll(ser, 300.0)
        self.assertEqual(len(ser.written), 3)

    def test_ignores_bad_commands(self):
        opener = FakeOpener([{"device": DEV, "command_id": 1, "hex": "XYZ"}, {"device": DEV}])
        lora, ser = pe.LoraBridge("http://x", self.token, opener), FakeSerial()
        lora.poll(ser, 100.0)
        self.assertEqual(ser.written, [])


if __name__ == "__main__":
    unittest.main()
