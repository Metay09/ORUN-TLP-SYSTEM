"""1d/1e: uygulamadan tasma ekle — çiftliğin her gateway'i, tek kurulumda."""
from __future__ import annotations

import http.client
import json
import os
from http.server import ThreadingHTTPServer
from pathlib import Path
import tempfile
import threading
import unittest

from backend.account_login import AccountStore
from backend.device_setup import SetupRefused, confirm_tracker, prepare_tracker
from backend.orun_backend_bench import oc
from backend.tests import test_account_device_login as accounts_base
from tools.orun_shared_sync_loopback import prepare
from tools.orun_sunucu import build

PASSWORD = accounts_base.PASSWORD
G1, G2 = 0x09A462BD4B275BA5, 0x1111222233334444
TRK = 0x0E8ADE7E71531AA3


def report(cred: str = "none", epoch: int = 0, granted: str = "none", gen: int = 0) -> dict:
    return {"device": f"{TRK:016X}", "credential": cred, "epoch": epoch,
            "granted": granted, "generation": gen, "floor": 0}


class PlanTests(unittest.TestCase):
    def setUp(self) -> None:
        tmp = tempfile.TemporaryDirectory(prefix="orun-tasma-")
        self.addCleanup(tmp.cleanup)
        self.reg = oc.CredentialRegistry(Path(tmp.name) / "reg.sqlite")

    def test_new_tracker_gets_every_farm_gateway_then_a_later_one(self):
        with self.assertRaises(SetupRefused) as ctx:
            prepare_tracker(self.reg, report())
        self.assertEqual(ctx.exception.code, "no_gateway")

        self.reg.note_gateway(G1)
        out = prepare_tracker(self.reg, report())
        self.assertEqual(out["plan"], [{"gateway": f"{G1:016X}", "generation": 1}])
        self.assertTrue(out["lines"][0].startswith("BENCH PROVISION "))
        cred = self.reg.active(TRK)
        cid = cred.credential_id.hex().upper()
        done = confirm_tracker(self.reg, report(cid, cred.key_epoch, f"{G1:016X}", 1))
        self.assertEqual(done["gateways"], [f"{G1:016X}"])
        self.assertEqual(self.reg.grants_for_gateway(G1)[0][0].device_id, TRK)   # 1c paketine girer

        again = prepare_tracker(self.reg, report(cid, cred.key_epoch, f"{G1:016X}", 1))
        self.assertEqual(again["lines"], [])                                     # zaten hesapta

        self.reg.note_gateway(G2)                                                # yeni gateway
        more = prepare_tracker(self.reg, report(cid, cred.key_epoch, f"{G1:016X}", 1))
        self.assertEqual(more["plan"], [{"gateway": f"{G2:016X}", "generation": 2}])
        done = confirm_tracker(self.reg, report(cid, cred.key_epoch, f"{G1:016X}", 1))
        self.assertEqual(done["gateways"], sorted([f"{G1:016X}", f"{G2:016X}"]))

    def test_refusals(self):
        self.reg.note_gateway(G1)
        with self.assertRaises(SetupRefused) as ctx:
            prepare_tracker(self.reg, report("AB" * 16, 1))                       # başka anahtar
        self.assertEqual(ctx.exception.code, "foreign_credential")
        prepare_tracker(self.reg, report())
        cred = self.reg.active(TRK)
        with self.assertRaises(SetupRefused) as ctx:                              # yanlış nesil
            confirm_tracker(self.reg, report(cred.credential_id.hex().upper(),
                                             cred.key_epoch, f"{G1:016X}", 7))
        self.assertEqual(ctx.exception.code, "generation_mismatch")


class HttpTests(unittest.TestCase):
    def setUp(self) -> None:
        tmp = tempfile.TemporaryDirectory(prefix="orun-tasma-http-")
        self.addCleanup(tmp.cleanup)
        root = Path(tmp.name)
        state = root / "state"
        state.mkdir(mode=0o700)
        prepare(state, reuse_state=False)
        AccountStore(state / "owner_accounts.sqlite").bootstrap_owner(
            "orun-main", "owner", "ownerlogin", PASSWORD)
        os.chmod(state / "owner_accounts.sqlite", 0o600)
        local, handler = build(state, root / "ledger.sqlite", root / "reg.sqlite", "orun-main")
        local.backend.registry.note_gateway(G1)
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), handler)
        self.server.daemon_threads = True
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.addCleanup(self.server.server_close)
        self.addCleanup(self.server.shutdown)

    def post(self, path: str, body: dict, bearer: str | None = None) -> tuple[int, dict]:
        conn = http.client.HTTPConnection("127.0.0.1", self.server.server_port, timeout=10)
        headers = {"Content-Type": "application/json"}
        if bearer:
            headers["Authorization"] = "Bearer " + bearer
        try:
            conn.request("POST", path, body=json.dumps(body), headers=headers)
            r = conn.getresponse()
            return r.status, json.loads(r.read())
        finally:
            conn.close()

    def test_owner_only(self):
        self.assertEqual(self.post("/api/v1/devices/tracker/prepare", report())[0], 401)
        token = self.post("/api/v1/account/login",
                          {"username": "ownerlogin", "password": PASSWORD})[1]["account_access_token"]
        status, body = self.post("/api/v1/devices/tracker/prepare", report(), token)
        self.assertEqual(status, 200)
        self.assertEqual(len(body["lines"]), 1)
        status, body = self.post("/api/v1/devices/tracker/prepare", report("AB" * 16, 1), token)
        self.assertEqual((status, body["error"]), (409, "foreign_credential"))


if __name__ == "__main__":
    unittest.main()
