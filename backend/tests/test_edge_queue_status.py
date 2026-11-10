"""End-to-end authorization and durability checks for PC Edge queue telemetry."""
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
from backend.edge_queue_status import (
    BadEdgeQueueStatus, EdgeQueueStatus, parse_edge_queue_report,
)
from backend.tests.test_account_device_login import PASSWORD
from tools.orun_shared_sync_loopback import prepare
from tools.orun_sunucu import bridge_handler, build


TOKEN = "SYNTHETIC-BRIDGE-TOKEN-1234567890"


class EdgeQueueStatusTests(unittest.TestCase):
    def test_validated_counts_and_durable_server_clock(self):
        with tempfile.TemporaryDirectory() as root:
            store = EdgeQueueStatus(Path(root) / "state.sqlite", clock=lambda: 1_800_000_000)
            store.report("pc-a", 7, 42, 1)
            self.assertEqual(store.list(), [{
                "edge": "pc-a", "pending": 7, "delivered": 42,
                "quarantined": 1, "seen_utc": 1_800_000_000,
            }])
            EdgeQueueStatus(Path(root) / "state.sqlite", clock=lambda: 1_800_000_120).report(
                "pc-a", 0, 49, 1)
            row = store.list()[0]
            self.assertEqual((row["pending"], row["delivered"], row["seen_utc"]),
                             (0, 49, 1_800_000_120))
            for item in (
                {"edge": "pc-a", "pending": True, "delivered": 4, "quarantined": 0},
                {"edge": "pc-a", "pending": -1, "delivered": 4, "quarantined": 0},
                {"edge": "../bad", "pending": 0, "delivered": 4, "quarantined": 0},
                {"edge": "pc-a", "pending": 0, "delivered": 4},
                {"edge": "pc-a", "pending": 0, "delivered": 4, "quarantined": 0, "token": "no"},
            ):
                with self.assertRaises(BadEdgeQueueStatus):
                    parse_edge_queue_report(item)
            self.assertEqual(store.list()[0], row)


class EdgeQueueApiTests(unittest.TestCase):
    def setUp(self):
        root = tempfile.TemporaryDirectory(prefix="orun-edge-counts-")
        self.addCleanup(root.cleanup)
        folder = Path(root.name)
        state = folder / "state"
        state.mkdir(mode=0o700)
        prepare(state, reuse_state=False)
        account_db = state / "owner_accounts.sqlite"
        AccountStore(account_db).bootstrap_owner("orun-main", "owner", "ownerlogin", PASSWORD)
        os.chmod(account_db, 0o600)
        local, handler = build(state, folder / "ledger.sqlite",
                               folder / "registry.sqlite", "orun-main",
                               maps_dir=folder / "maps")
        self.account = self._serve(handler)
        self.bridge = self._serve(bridge_handler(local, TOKEN))

    def _serve(self, handler):
        server = ThreadingHTTPServer(("127.0.0.1", 0), handler)
        server.daemon_threads = True
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(thread.join, 5)
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        return server

    @staticmethod
    def _req(server, method, path, token=None, body=None):
        conn = http.client.HTTPConnection("127.0.0.1", server.server_port, timeout=10)
        headers = {}
        if token:
            headers["Authorization"] = "Bearer " + token
        data = None
        if body is not None:
            headers["Content-Type"] = "application/json"
            data = json.dumps(body).encode()
        try:
            conn.request(method, path, body=data, headers=headers)
            resp = conn.getresponse()
            return resp.status, json.loads(resp.read())
        finally:
            conn.close()

    def _login(self):
        status, body = self._req(
            self.account, "POST", "/api/v1/account/login",
            body={"username": "ownerlogin", "password": PASSWORD})
        self.assertEqual(status, 200)
        return body["account_access_token"]

    def test_status_read_requires_site_owner_and_ingest_requires_bridge(self):
        sample = {"edge": "pc-a", "pending": 8, "delivered": 21, "quarantined": 1}
        read = "/api/v1/devices/edge-status"
        write = "/v1/bridge/edge-status"
        self.assertEqual(self._req(self.account, "GET", read)[0], 401)
        self.assertEqual(self._req(self.bridge, "POST", write, body=sample)[0], 401)
        self.assertEqual(self._req(self.bridge, "POST", write, "wrong-bearer", sample)[0], 401)
        self.assertEqual(self._req(self.account, "GET", read, self._login()),
                         (200, {"edges": []}))
        for invalid in (dict(sample, pending=-1), dict(sample, quarantined=True),
                        dict(sample, edge="invalid/id")):
            self.assertEqual(self._req(self.bridge, "POST", write, TOKEN, invalid)[0], 400)
        self.assertEqual(self._req(self.bridge, "POST", write, TOKEN, sample)[0], 200)
        token = self._login()
        status, body = self._req(self.account, "GET", read, token)
        self.assertEqual(status, 200)
        self.assertEqual(len(body["edges"]), 1)
        row = body["edges"][0]
        self.assertEqual((row["edge"], row["pending"], row["delivered"], row["quarantined"]),
                         ("pc-a", 8, 21, 1))
        self.assertGreater(row["seen_utc"], 1_700_000_000)


if __name__ == "__main__":
    unittest.main()
