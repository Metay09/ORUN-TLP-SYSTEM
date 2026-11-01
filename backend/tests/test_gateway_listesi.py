"""2c: Bağlantı ekranı — çiftliğin gateway'leri, nasıl bağlı, bellek, kaç tasma."""
from __future__ import annotations

import http.client
import json
import os
from http.server import ThreadingHTTPServer
from pathlib import Path
import sqlite3
import tempfile
import threading
import time
import unittest

from backend.account_login import AccountStore
from backend.gateway_status import GatewayStatus, parse_report, BadStatus
from backend.tests.test_account_device_login import PASSWORD
from tools.orun_shared_sync_loopback import prepare
from tools.orun_sunucu import bridge_handler, build

TOKEN = "SYNTHETIC-BRIDGE-TOKEN-1234567890"
GW_PC = "09A462BD4B275BA5"
GW_PHONE = "1122334455667788"
GW_OLD = "AABBCCDDEEFF0011"


class GatewayListTests(unittest.TestCase):
    def setUp(self) -> None:
        tmp = tempfile.TemporaryDirectory(prefix="orun-gw-")
        self.addCleanup(tmp.cleanup)
        root = Path(tmp.name)
        state = root / "state"
        state.mkdir(mode=0o700)
        prepare(state, reuse_state=False)
        accounts_db = state / "owner_accounts.sqlite"
        AccountStore(accounts_db).bootstrap_owner("orun-main", "owner", "ownerlogin", PASSWORD)
        os.chmod(accounts_db, 0o600)
        self.db = root / "ledger.sqlite"
        local, handler = build(state, self.db, root / "registry.sqlite", "orun-main",
                               maps_dir=root / "maps")
        self.local = local
        self.account = self.start(handler)
        self.bridge = self.start(bridge_handler(local, TOKEN))

    def start(self, handler) -> ThreadingHTTPServer:
        server = ThreadingHTTPServer(("127.0.0.1", 0), handler)
        server.daemon_threads = True
        t = threading.Thread(target=server.serve_forever, daemon=True)
        t.start()
        self.addCleanup(t.join, 5)
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        return server

    def req(self, server, method, path, bearer=None, body=None):
        conn = http.client.HTTPConnection("127.0.0.1", server.server_port, timeout=10)
        headers = {}
        raw = None
        if bearer:
            headers["Authorization"] = "Bearer " + bearer
        if body is not None:
            raw = json.dumps(body).encode()
            headers["Content-Type"] = "application/json"
        try:
            conn.request(method, path, body=raw, headers=headers)
            reply = conn.getresponse()
            return reply.status, json.loads(reply.read() or b"{}")
        finally:
            conn.close()

    def login(self) -> str:
        status, body = self.req(self.account, "POST", "/api/v1/account/login",
                                body={"username": "ownerlogin", "password": PASSWORD})
        self.assertEqual(status, 200)
        return body["account_access_token"]

    def test_bridge_and_phone_reports_make_the_farm_list(self):
        pc = {"gateway": GW_PC, "transport": "usb", "held": 2, "capacity": 1056}
        self.assertEqual(self.req(self.bridge, "POST", "/v1/bridge/gateway", body=pc)[0], 401)
        self.assertEqual(self.req(self.bridge, "POST", "/v1/bridge/gateway", TOKEN, pc)[0], 200)

        token = self.login()
        phone = {"gateway": GW_PHONE, "transport": "ble", "held": 0, "capacity": 1056}
        self.assertEqual(self.req(self.account, "POST", "/api/v1/devices/gateway", body=phone)[0], 401)
        self.assertEqual(self.req(self.account, "POST", "/api/v1/devices/gateway", token, phone)[0], 200)
        bad = dict(phone, transport="wifi")
        self.assertEqual(self.req(self.account, "POST", "/api/v1/devices/gateway", token, bad)[0], 400)

        # Kayıtlar: PC'deki gateway'den son 24 saatte iki ayrı tasma, biri eski.
        now = int(time.time())
        with sqlite3.connect(self.db) as db:
            for dev, t in ((b"\x01" * 8, now - 60), (b"\x02" * 8, now - 3600), (b"\x03" * 8, now - 3 * 86400)):
                db.execute("INSERT INTO backend_uploads VALUES (?,?,?,?,?,?)",
                           (dev, b"\x00" * 8, 1, "pc-edge", GW_PC, t))
            db.execute("INSERT INTO backend_uploads VALUES (?,?,?,?,?,?)",
                       (b"\x04" * 8, b"\x00" * 8, 1, "pc-edge", GW_OLD, now - 7200))

        self.assertEqual(self.req(self.account, "GET", "/api/v1/devices/gateways")[0], 401)
        status, body = self.req(self.account, "GET", "/api/v1/devices/gateways", token)
        self.assertEqual(status, 200)
        rows = {g["gateway"]: g for g in body["gateways"]}
        self.assertEqual(rows[GW_PC]["via"], "pc-usb")
        self.assertEqual((rows[GW_PC]["held"], rows[GW_PC]["capacity"]), (2, 1056))
        self.assertEqual(rows[GW_PC]["trackers_24h"], 2)
        self.assertEqual(rows[GW_PHONE]["via"], "phone-ble")
        self.assertIsNone(rows[GW_OLD]["via"], "durum bildirmeyen gateway bağlı sayılmaz")
        self.assertEqual(rows[GW_OLD]["last_seen_utc"], now - 7200)
        # Bildirim yapan gateway çiftliğe de girer (izin paketi için).
        farm = {"%016X" % g for g in self.local.backend.registry.farm_gateways()}
        self.assertTrue({GW_PC, GW_PHONE} <= farm)

    def test_old_report_is_not_connected(self):
        clock = [1_000_000.0]
        store = GatewayStatus(self.db, clock=lambda: clock[0])
        store.report(GW_PC, "pc-usb", 1, 1056)
        self.assertEqual(store.list()[0]["via"], "pc-usb")
        clock[0] += 301
        self.assertIsNone(store.list()[0]["via"])
        self.assertEqual(store.list([GW_OLD])[-1]["gateway"], GW_OLD)

    def test_report_validation(self):
        for body in (None, {}, {"gateway": "XYZ", "transport": "usb"},
                     {"gateway": GW_PC, "transport": "usb", "held": 5, "capacity": 2},
                     {"gateway": GW_PC, "transport": "usb", "held": True}):
            with self.assertRaises(BadStatus):
                parse_report(body, "pc")
        self.assertEqual(parse_report({"gateway": GW_PC.lower(), "transport": "ble"}, "phone"),
                         (GW_PC, "phone-ble", None, None))


if __name__ == "__main__":
    unittest.main()
