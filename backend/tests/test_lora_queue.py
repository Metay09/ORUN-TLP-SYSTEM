"""6c: LoRa ayar kuyruğu — işlemden imzalı komuta, tasmanın cevabından "işlendi"ye."""
import hashlib
import hmac
import struct
import tempfile
import unittest
from pathlib import Path

from backend import lora_command as lc
from backend import orun_credentials as oc
from backend.device_ops import DeviceOps
from backend.device_settings import DeviceSettings
from backend.lora_queue import BadEvent, LoraQueue, entries_for

DEV = 0x52E16773141A8887
DEV_HEX = f"{DEV:016X}"


class FakeTracker:
    """Answers like firmware LoraCommandService (signature, state token, STALE)."""

    def __init__(self, cred):
        self.key = oc.ble_write_key(cred)
        self.cred = cred
        self.inc, self.rev = 0x0BADC0FFEE123456, 7
        self.cfg = {1: 180, 2: 1 | (1 << 8), 3: 30}
        self.last = None

    def answer(self, frame: bytes) -> bytes:
        body, tag = frame[:-16], frame[-16:]
        assert hmac.compare_digest(hmac.new(self.key, lc.COMMAND_DOMAIN + body, hashlib.sha256).digest()[:16], tag)
        cid = struct.unpack(">I", body[16:20])[0]
        inc, rev = struct.unpack(">QI", body[20:32])
        n = body[32]
        entries = [struct.unpack(">BI", body[33 + 5 * i: 38 + 5 * i]) for i in range(n)]
        if (inc, rev) != (self.inc, self.rev):
            outcome = 7  # STALE
        elif all(self.cfg[i] == v for i, v in entries):
            outcome = 1
        else:
            for i, v in entries:
                self.cfg[i] = v
            self.rev += 1
            outcome = 0
        out = struct.pack(">BBBBQIIQIIHBB", 2, 8, outcome, 1, self.cred.device_id, self.cred.key_epoch, cid,
                          self.inc, self.rev, self.cfg[1], self.cfg[3], self.cfg[2] & 0xFF, self.cfg[2] >> 8)
        return out + hmac.new(self.key, lc.RESULT_DOMAIN + out, hashlib.sha256).digest()[:16]


class LoraQueueTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        d = Path(self.tmp.name)
        self.reg = oc.CredentialRegistry(d / "reg.sqlite")
        self.cred = self.reg.create(DEV)
        db = d / "db.sqlite"
        self.ops, self.settings = DeviceOps(db), DeviceSettings(db)
        self.q = LoraQueue(db, lambda: self.reg, self.ops, self.settings)
        self.tracker = FakeTracker(self.cred)

    def tearDown(self):
        self.tmp.cleanup()

    def op_state(self, op):
        return next(r for r in self.ops.recent() if r["op"] == op and r["device"] == DEV_HEX)

    def deliver(self):
        """One report: the bridge hands the queued command, the tracker answers."""
        (cmd,) = self.q.commands()
        self.q.events({"events": [{"kind": "queued", "device": cmd["device"], "command_id": cmd["command_id"]}]})
        res = self.tracker.answer(bytes.fromhex(cmd["hex"]))
        return self.q.events({"events": [{"kind": "result", "hex": res.hex().upper()}]})["results"][0]

    def test_probe_then_apply(self):
        self.ops.create({"op": "op-a1b2", "kind": "settings",
                         "items": [{"device": DEV_HEX, "payload": {"set": {"interval_s": 300}}}]})
        self.assertTrue(self.q.enqueue("op-a1b2", DEV_HEX, {"set": {"interval_s": 300}}))
        self.assertTrue(self.q.commands()[0]["probe"])  # state not known yet
        self.assertEqual(self.deliver(), "resigned")    # STALE carried the state
        self.assertEqual(self.op_state("op-a1b2")["state"], "gateway")
        self.assertFalse(self.q.commands()[0]["probe"])
        self.assertEqual(self.deliver(), "applied")
        self.assertEqual(self.op_state("op-a1b2")["state"], "applied")
        self.assertEqual(self.op_state("op-a1b2")["via"], "lora")
        self.assertEqual(self.q.commands(), [])
        rep = self.settings.all()[DEV_HEX]
        self.assertEqual((rep["reported"]["interval_s"], rep["reported_via"]), (300, "lora"))
        # The state is known now: the next change goes straight.
        self.ops.create({"op": "op-c3d4", "kind": "settings",
                         "items": [{"device": DEV_HEX, "payload": {"set": {"ble_window_s": 60}}}]})
        self.q.enqueue("op-c3d4", DEV_HEX, {"set": {"ble_window_s": 60}})
        self.assertFalse(self.q.commands()[0]["probe"])
        self.assertEqual(self.deliver(), "applied")
        self.assertEqual(self.tracker.cfg[3], 60)

    def test_changed_meanwhile_and_cancel(self):
        self.ops.create({"op": "op-e5f6", "kind": "settings",
                         "items": [{"device": DEV_HEX, "payload": {"set": {"interval_s": 600}}}]})
        self.q.enqueue("op-e5f6", DEV_HEX, {"set": {"interval_s": 600}})
        self.deliver()  # probe
        self.tracker.rev += 1  # changed over Bluetooth in between
        self.assertEqual(self.deliver(), "resigned")
        self.assertEqual(self.deliver(), "applied")
        # Settled over USB/Bluetooth first: the LoRa command goes away.
        self.ops.create({"op": "op-a7b8", "kind": "settings",
                         "items": [{"device": DEV_HEX, "payload": {"set": {"interval_s": 900}}}]})
        self.q.enqueue("op-a7b8", DEV_HEX, {"set": {"interval_s": 900}})
        self.q.cancel("op-a7b8", DEV_HEX)
        self.assertEqual(self.q.commands(), [])

    def test_forged_or_foreign_answers_change_nothing(self):
        self.ops.create({"op": "op-f9e8", "kind": "settings",
                         "items": [{"device": DEV_HEX, "payload": {"set": {"interval_s": 300}}}]})
        self.q.enqueue("op-f9e8", DEV_HEX, {"set": {"interval_s": 300}})
        cmd = self.q.commands()[0]
        res = bytearray(self.tracker.answer(bytes.fromhex(cmd["hex"])))
        res[-1] ^= 1
        self.assertEqual(self.q.events({"events": [{"kind": "result", "hex": res.hex().upper()}]})["results"],
                         ["bad_signature"])
        self.assertEqual(self.q.commands()[0]["command_id"], cmd["command_id"])
        self.assertEqual(self.q.events({"events": [{"kind": "queued", "device": DEV_HEX, "command_id": 1}]})
                         ["results"], ["unknown"])
        for bad in ({}, {"events": []}, {"events": [{"kind": "x"}]}, {"events": [{"kind": "result", "hex": "00"}]},
                    {"events": [{"kind": "sent", "device": "xyz", "command_id": 1}]}):
            with self.assertRaises(BadEvent):
                self.q.events(bad)
        # No key for a device (a gateway): nothing queued.
        self.assertFalse(self.q.enqueue("op-f9e8", "09A462BD4B275BA5", {"set": {"interval_s": 300}}))

    def test_rejected(self):
        self.ops.create({"op": "op-1234", "kind": "settings",
                         "items": [{"device": DEV_HEX, "payload": {"set": {"interval_s": 300}}}]})
        self.q.enqueue("op-1234", DEV_HEX, {"set": {"interval_s": 300}})
        self.deliver()
        cmd = self.q.commands()[0]
        frame = bytes.fromhex(cmd["hex"])
        # The tracker says INVALID (e.g. a value it does not take).
        cid = struct.unpack(">I", frame[16:20])[0]
        t = self.tracker
        out = struct.pack(">BBBBQIIQIIHBB", 2, 8, 2, 1, t.cred.device_id, t.cred.key_epoch, cid,
                          t.inc, t.rev, 180, 30, 1, 1)
        res = out + hmac.new(t.key, lc.RESULT_DOMAIN + out, hashlib.sha256).digest()[:16]
        self.assertEqual(self.q.events({"events": [{"kind": "result", "hex": res.hex()}]})["results"], ["rejected"])
        self.assertEqual(self.op_state("op-1234")["state"], "rejected")

    def test_entries(self):
        self.assertEqual(entries_for({"interval_s": 300, "ble_window_s": 0}, None), [(1, 300), (3, 0)])
        self.assertEqual(entries_for({"relay": True}, {"tracking": False}), [(2, 1 | (2 << 8))])
        self.assertEqual(entries_for({"tracking": True}, None), [(2, 1 | (1 << 8))])
        self.assertEqual(entries_for({}, None), [])


if __name__ == "__main__":
    unittest.main()
