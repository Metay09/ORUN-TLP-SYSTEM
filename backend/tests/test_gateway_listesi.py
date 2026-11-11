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
        self.assertEqual(rows[GW_PC]["last_upload_utc"], now - 60)
        self.assertLessEqual(abs(rows[GW_PC]["status_seen_utc"] - now), 2)
        self.assertIsNone(rows[GW_OLD]["status_seen_utc"],
                          "an upload is not a fresh gateway memory sample")
        self.assertIsNone(rows[GW_PHONE]["last_upload_utc"])
        # Bildirim yapan gateway çiftliğe de girer (izin paketi için).
        farm = {"%016X" % g for g in self.local.backend.registry.farm_gateways()}
        self.assertTrue({GW_PC, GW_PHONE} <= farm)

    def test_device_names_live_in_the_account(self):
        """2d: ad hesapta; boş ad siler; geçersiz ad reddedilir."""
        self.assertEqual(self.req(self.account, "GET", "/api/v1/devices/names")[0], 401)
        token = self.login()
        ok = self.req(self.account, "POST", "/api/v1/devices/name", token,
                      {"device": GW_PC.lower(), "name": "  Tepe   gateway "})
        self.assertEqual(ok, (200, {"device": GW_PC, "name": "Tepe gateway"}))
        self.req(self.account, "POST", "/api/v1/devices/name", token,
                 {"device": "0E8ADE7E71531AA3", "name": "Sarıkız"})
        self.assertEqual(self.req(self.account, "GET", "/api/v1/devices/names", token)[1]["names"],
                         {GW_PC: "Tepe gateway", "0E8ADE7E71531AA3": "Sarıkız"})
        self.req(self.account, "POST", "/api/v1/devices/name", token, {"device": GW_PC, "name": ""})
        self.assertEqual(self.req(self.account, "GET", "/api/v1/devices/names", token)[1]["names"],
                         {"0E8ADE7E71531AA3": "Sarıkız"})
        for bad in ({"device": "xyz", "name": "a"}, {"device": GW_PC, "name": "a" * 41},
                    {"device": GW_PC, "name": "a\u0007b"}, {"device": GW_PC}):
            self.assertEqual(self.req(self.account, "POST", "/api/v1/devices/name", token, bad)[0], 400, bad)

    def test_battery_capacity_lives_in_the_account(self):
        """Y3: pil kapasitesi hesapta (tasmaya yazılmaz); None siler."""
        self.assertEqual(self.req(self.account, "GET", "/api/v1/devices/battery")[0], 401)
        token = self.login()
        ok = self.req(self.account, "POST", "/api/v1/devices/battery", token,
                      {"device": "0e8ade7e71531aa3", "mah": 3200})
        self.assertEqual(ok, (200, {"device": "0E8ADE7E71531AA3", "mah": 3200}))
        self.req(self.account, "POST", "/api/v1/devices/battery", token,
                 {"device": "52E16773141A8887", "mah": 6000})
        self.assertEqual(self.req(self.account, "GET", "/api/v1/devices/battery", token)[1]["battery"],
                         {"0E8ADE7E71531AA3": 3200, "52E16773141A8887": 6000})
        self.req(self.account, "POST", "/api/v1/devices/battery", token,
                 {"device": "52E16773141A8887", "mah": None})
        self.assertEqual(self.req(self.account, "GET", "/api/v1/devices/battery", token)[1]["battery"],
                         {"0E8ADE7E71531AA3": 3200})
        for bad in ({"device": "xyz", "mah": 1000}, {"device": GW_PC, "mah": 10},
                    {"device": GW_PC, "mah": 60000}, {"device": GW_PC, "mah": "1000"},
                    {"device": GW_PC, "mah": True}, {"device": GW_PC, "mah": 12.5}):
            self.assertEqual(self.req(self.account, "POST", "/api/v1/devices/battery", token, bad)[0], 400, bad)

    def test_tracker_fences_live_in_the_account(self):
        """3a: tasmaya yazılan alanların özeti hesapta."""
        self.assertEqual(self.req(self.account, "GET", "/api/v1/devices/fences")[0], 401)
        token = self.login()
        body = {"device": "0e8ade7e71531aa3", "digest": "0123456789abcdef", "areas": 2, "vertices": 9,
                "zones": [{"id": "z1", "name": "Ev merası"}, {"id": "z2", "name": "Taşlık"}]}
        status, out = self.req(self.account, "POST", "/api/v1/devices/fence", token, body)
        self.assertEqual((status, out["device"]), (200, "0E8ADE7E71531AA3"))
        got = self.req(self.account, "GET", "/api/v1/devices/fences", token)[1]["fences"]["0E8ADE7E71531AA3"]
        self.assertEqual((got["digest"], got["areas"], got["vertices"], got["zones"][0]["name"]),
                         ("0123456789abcdef", 2, 9, "Ev merası"))
        for bad in (dict(body, areas=0), dict(body, areas=9), dict(body, digest="xyz"),
                    dict(body, zones=[{"id": "z1", "name": "a"}]), dict(body, device="bad")):
            self.assertEqual(self.req(self.account, "POST", "/api/v1/devices/fence", token, bad)[0], 400)
        # Alanlar tasmadan silinince (APP FENCE CLEAR): 0 alan, 0 köşe, boş liste.
        cleared = dict(body, digest="e3b0c44298fc1c14", areas=0, vertices=0, zones=[])
        self.assertEqual(self.req(self.account, "POST", "/api/v1/devices/fence", token, cleared)[0], 200)
        got = self.req(self.account, "GET", "/api/v1/devices/fences", token)[1]["fences"]["0E8ADE7E71531AA3"]
        self.assertEqual((got["areas"], got["vertices"], got["zones"]), (0, 0, []))
        self.assertEqual(self.req(self.account, "POST", "/api/v1/devices/fence", token,
                                  dict(cleared, vertices=3))[0], 400)

    def test_device_settings_live_in_the_account(self):
        """4b: istenen ayar ve cihazdan okunan değer hesapta; toplu yazılabilir."""
        self.assertEqual(self.req(self.account, "GET", "/api/v1/devices/settings")[0], 401)
        token = self.login()
        a, b = "0e8ade7e71531aa3", "0E8ADE7E71538B3C"
        status, out = self.req(self.account, "POST", "/api/v1/devices/settings", token,
                               {"devices": [a, b], "set": {"interval_s": 900}})
        self.assertEqual((status, out["devices"]), (200, [a.upper(), b]))
        self.req(self.account, "POST", "/api/v1/devices/settings", token, {"devices": [b], "set": {"relay": True}})
        status, _ = self.req(self.account, "POST", "/api/v1/devices/settings/reported", token,
                             {"device": b, "values": {"interval_s": 180, "tracking": True, "relay": False}, "via": "usb"})
        self.assertEqual(status, 200)
        got = self.req(self.account, "GET", "/api/v1/devices/settings", token)[1]["settings"]
        self.assertEqual(got[a.upper()]["wanted"], {"interval_s": 900})
        self.assertIsNone(got[a.upper()]["reported"])
        self.assertEqual(got[b]["wanted"], {"interval_s": 900, "relay": True})
        self.assertEqual((got[b]["reported"]["interval_s"], got[b]["reported_via"]), (180, "usb"))
        for bad in ({"devices": [a], "set": {"interval_s": 59}}, {"devices": [a], "set": {"interval_s": True}},
                    {"devices": [a], "set": {"tracking": 1}}, {"devices": [a], "set": {"colour": "red"}},
                    {"devices": [a], "set": {}}, {"devices": [], "set": {"relay": True}},
                    {"devices": ["bad"], "set": {"relay": True}},
                    {"devices": [a], "set": {"ble_window_s": 301}}, {"devices": [a], "set": {"ble_window_s": True}},
                    {"devices": [a], "set": {"ble_window_s": -1}}):
            self.assertEqual(self.req(self.account, "POST", "/api/v1/devices/settings", token, bad)[0], 400)
        # 4c-1a: Bluetooth after each report, 0 (off) .. 300 s.
        for ok in (0, 30, 300):
            self.assertEqual(self.req(self.account, "POST", "/api/v1/devices/settings", token,
                                      {"devices": [b], "set": {"ble_window_s": ok}})[0], 200)
        self.assertEqual(self.req(self.account, "POST", "/api/v1/devices/settings/reported", token,
                                  {"device": b, "values": {"ble_window_s": 30}, "via": "usb"})[0], 200)
        self.assertEqual(self.req(self.account, "POST", "/api/v1/devices/settings/reported", token,
                                  {"device": b, "values": {"relay": True}, "via": "wifi"})[0], 400)

    def test_bluetooth_write_keys_for_the_account_phones(self):
        # 4c-1b: shared vector with firmware/tests/m7/test_ble_write.cpp and
        # edge/app/test/bleYazma.test.ts.
        from backend import orun_credentials as oc
        cred = oc.Credential(0x0E8ADE7E71531AA3, bytes(range(0x10, 0x20)), 1,
                             bytes(range(0x40, 0x60)), False)
        self.assertEqual(oc.ble_write_key(cred).hex().upper(),
                         "75BF1BD3052DFA4CD5E9DB9DD084B98DB9622C2FABE44588FB58E02EC0952DD2")
        self.assertNotEqual(oc.ble_write_key(cred), oc.traffic_key(cred))
        registry = self.local.backend.registry
        made = registry.create(0x52E16773141A8887)
        self.assertEqual(self.req(self.account, "GET", "/api/v1/devices/write-keys")[0], 401)
        status, body = self.req(self.account, "GET", "/api/v1/devices/write-keys", self.login())
        self.assertEqual(status, 200)
        mine = [k for k in body["keys"] if k["device"] == "52E16773141A8887"]
        self.assertEqual(mine, [{"device": "52E16773141A8887", "epoch": made.key_epoch,
                                 "write_key": oc.ble_write_key(made).hex().upper()}])
        self.assertNotIn(made.k_root.hex().upper(), json.dumps(body))
        # A replaced credential's old epoch is no longer handed out.
        newer = registry.create(0x52E16773141A8887, replace=True)
        body = self.req(self.account, "GET", "/api/v1/devices/write-keys", self.login())[1]
        self.assertEqual([k["epoch"] for k in body["keys"] if k["device"] == "52E16773141A8887"],
                         [newer.key_epoch])

    def test_device_ops_in_the_account(self):
        """İşlemler: hedef başına durum; yalnız cihaz onayı 'işlendi' yapar; yenisi eskisinin yerine geçer."""
        self.assertEqual(self.req(self.account, "GET", "/api/v1/devices/ops")[0], 401)
        token = self.login()
        a, b = "0E8ADE7E71531AA3", "0E8ADE7E71538B3C"
        op1 = {"op": "op-aaaa1", "kind": "fence", "items": [
            {"device": a, "payload": {"digest": "0123456789abcdef", "areas": 1}},
            {"device": b.lower(), "payload": {"digest": "fedcba9876543210", "areas": 2}}]}
        self.assertEqual(self.req(self.account, "POST", "/api/v1/devices/ops", token, op1)[1]["created"], True)
        self.assertEqual(self.req(self.account, "POST", "/api/v1/devices/ops", token, op1)[1]["created"], False)
        post = lambda body: self.req(self.account, "POST", "/api/v1/devices/ops/result", token, body)
        self.assertEqual(post({"op": "op-aaaa1", "device": a, "state": "applied", "via": "usb"})[1]["state"], "applied")
        # a gateway note never goes back over the device's own answer
        self.assertEqual(post({"op": "op-aaaa1", "device": a, "state": "gateway", "via": "lora"})[1]["state"], "applied")
        # "gateway" exists only on the LoRa path
        self.assertEqual(post({"op": "op-aaaa1", "device": b, "state": "gateway", "via": "usb"})[0], 400)
        # a newer fence for b replaces the waiting one
        self.req(self.account, "POST", "/api/v1/devices/ops", token,
                 {"op": "op-bbbb2", "kind": "fence", "items": [{"device": b, "payload": {"digest": "1111111111111111"}}]})
        rows = {(r["op"], r["device"]): r for r in self.req(self.account, "GET", "/api/v1/devices/ops", token)[1]["ops"]}
        self.assertEqual(rows[("op-aaaa1", a)]["state"], "applied")
        self.assertEqual(rows[("op-aaaa1", a)]["via"], "usb")
        self.assertEqual(rows[("op-aaaa1", b)]["state"], "superseded")
        self.assertEqual(rows[("op-bbbb2", b)]["state"], "waiting")
        for bad in ({"op": "x", "kind": "fence", "items": [{"device": a, "payload": {}}]},
                    {"op": "op-cccc3", "kind": "valve", "items": [{"device": a, "payload": {}}]},
                    {"op": "op-cccc3", "kind": "fence", "items": []},
                    {"op": "op-cccc3", "kind": "fence", "items": [{"device": a, "payload": {}}, {"device": a, "payload": {}}]}):
            self.assertEqual(self.req(self.account, "POST", "/api/v1/devices/ops", token, bad)[0], 400)
        self.assertEqual(post({"op": "op-zzzz9", "device": a, "state": "applied", "via": "usb"})[0], 400)

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
