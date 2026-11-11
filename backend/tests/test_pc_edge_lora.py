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
        opener = FakeOpener([{"device": DEV, "resource": 1, "command_id": 7, "frames": [HEX]}])
        lora, ser = pe.LoraBridge("http://x", self.token, opener), FakeSerial()
        lora.poll(ser, 100.0)
        self.assertEqual(ser.written, [f"APP LORA QUEUE {HEX}\n"])  # 108 hex: one line
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

    def test_one_command_per_tracker_at_a_time(self):
        # The gateway holds one command per tracker: settings and areas must
        # not replace each other there. The oldest goes; the next follows.
        other = "0E8A00000000ABCD"
        opener = FakeOpener([{"device": DEV, "resource": 1, "command_id": 7, "frames": [HEX]},
                             {"device": DEV, "resource": 2, "command_id": 8, "frames": ["CD" * 54]},
                             {"device": other, "resource": 2, "command_id": 9, "frames": ["EF" * 54]}])
        lora, ser = pe.LoraBridge("http://x", self.token, opener), FakeSerial()
        lora.poll(ser, 100.0)
        self.assertEqual(ser.written, [f"APP LORA QUEUE {HEX}\n", f"APP LORA QUEUE {'EF' * 54}\n"])
        opener.commands = opener.commands[1:]   # the settings command was settled
        lora.poll(ser, 130.0)
        self.assertEqual(ser.written[-1], f"APP LORA QUEUE {'CD' * 54}\n")
        self.assertEqual(len(ser.written), 3)

    def test_ignores_bad_commands(self):
        opener = FakeOpener([{"device": DEV, "command_id": 1, "frames": ["XYZ"]}, {"device": DEV}])
        lora, ser = pe.LoraBridge("http://x", self.token, opener), FakeSerial()
        lora.poll(ser, 100.0)
        self.assertEqual(ser.written, [])


    def test_long_frames_go_in_pieces(self):
        frames = ["AB" * 255, "CD" * 255, "EF" * 85]
        opener = FakeOpener([{"device": DEV, "resource": 2, "command_id": 9, "frames": frames}])
        lora, ser = pe.LoraBridge("http://x", self.token, opener), FakeSerial()
        lora.poll(ser, 100.0)
        self.assertTrue(all(len(line) <= 160 for line in ser.written))
        joined, queued = "", []
        for line in ser.written:
            text = line.rstrip("\n")
            if text.startswith("APP LORA + "):
                joined += text[11:]
            else:
                queued.append(joined + text[len("APP LORA QUEUE "):])
                joined = ""
        self.assertEqual(queued, frames)
        self.assertEqual(len(lora.awaiting), 3)


if __name__ == "__main__":
    unittest.main()
