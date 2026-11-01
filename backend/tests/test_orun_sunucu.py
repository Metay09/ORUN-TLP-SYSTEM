"""1a: tek ORUN sunucusu — hesap + kayıt okuma + köprü yüklemesi tek süreçte."""
from __future__ import annotations

import http.client
import json
import os
from http.server import ThreadingHTTPServer
from pathlib import Path
import sqlite3
import tempfile
import threading
import unittest

from backend.account_login import AccountStore
from backend.tests.test_account_device_login import PASSWORD
from backend.tests.test_observation_latest import A, INC, periodic
from tools.orun_shared_sync_loopback import prepare
from tools.orun_sunucu import bridge_handler, build

BRIDGE_TOKEN = "SYNTHETIC-BRIDGE-TOKEN-1234567890"


class TekSunucuTests(unittest.TestCase):
    def setUp(self) -> None:
        tmp = tempfile.TemporaryDirectory(prefix="orun-sunucu-")
        self.addCleanup(tmp.cleanup)
        root = Path(tmp.name)
        state = root / "state"
        state.mkdir(mode=0o700)
        prepare(state, reuse_state=False)          # ortak veri + kayıtlı cihazlar
        accounts_db = state / "owner_accounts.sqlite"
        AccountStore(accounts_db).bootstrap_owner("orun-main", "owner", "ownerlogin", PASSWORD)
        os.chmod(accounts_db, 0o600)

        db = root / "ledger.sqlite"
        local, handler = build(state, db, root / "registry.sqlite", "orun-main")
        # Gerçek şemaya, doğrulamadan geçmiş gibi iki kayıt (aynı tasma).
        with sqlite3.connect(db) as c:
            for seq, t in ((1, 1_700_000_900), (2, 1_700_000_100)):
                c.execute("INSERT INTO verified_observations "
                          "(device_id,incarnation,sequence,family,plaintext,exact_first_frame) "
                          "VALUES (?,?,?,?,?,?)", (A, INC, seq, 1, periodic(t), bytes([seq]) * 100))
        self.local = local
        self.account = self.start(ThreadingHTTPServer(("127.0.0.1", 0), handler))
        self.bridge = self.start(ThreadingHTTPServer(
            ("127.0.0.1", 0), bridge_handler(local, BRIDGE_TOKEN)))

    def start(self, server: ThreadingHTTPServer) -> ThreadingHTTPServer:
        server.daemon_threads = True
        t = threading.Thread(target=server.serve_forever, daemon=True)
        t.start()
        self.addCleanup(t.join, 5)
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        return server

    def req(self, server, method, path, bearer=None, body=None):
        conn = http.client.HTTPConnection("127.0.0.1", server.server_port, timeout=10)
        headers = {"Accept": "application/json"}
        raw = None
        if bearer:
            headers["Authorization"] = "Bearer " + bearer
        if body is not None:
            raw = body if isinstance(body, bytes) else json.dumps(body).encode()
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

    def test_app_reads_latest_history_and_keys_from_one_process(self):
        self.assertEqual(self.req(self.account, "GET", "/v1/edge/records?latest=1")[0], 401)
        token = self.login()
        status, latest = self.req(self.account, "GET", "/v1/edge/records?latest=1", token)
        self.assertEqual(status, 200)
        self.assertEqual([r["sequence"] for r in latest["records"]], [1])  # en yeni ÖLÇÜM
        status, page = self.req(self.account, "GET", "/v1/edge/records?after=0", token)
        self.assertEqual((status, [r["sequence"] for r in page["records"]]), (200, [1, 2]))
        self.assertEqual(self.req(self.account, "GET", "/v1/edge/keys", token), (200, {"keys": []}))

    def test_bridge_uploads_only_with_its_token_and_cannot_read(self):
        self.assertEqual(self.req(self.bridge, "POST", "/v1/edge/batch", body={})[0], 401)
        status, body = self.req(self.bridge, "POST", "/v1/edge/batch", BRIDGE_TOKEN, body=b"not json")
        self.assertEqual(status, 400)                      # gerçekten Backend'e ulaştı
        self.assertEqual(self.req(self.bridge, "GET", "/v1/health", BRIDGE_TOKEN),
                         (200, {"ok": True, "records": 2}))
        self.assertEqual(self.req(self.bridge, "GET", "/v1/edge/records?latest=1", BRIDGE_TOKEN)[0], 404)
        # Köprü anahtarı uygulama tarafında yetki sayılmaz.
        self.assertEqual(self.req(self.account, "GET", "/v1/edge/records?latest=1", BRIDGE_TOKEN)[0], 401)


if __name__ == "__main__":
    unittest.main()
