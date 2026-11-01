"""Read-only sidecar and split relay acceptance tests; no live services."""
from __future__ import annotations

import http.client
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from threading import Thread
import tempfile
import unittest

from backend.account_observation_relay import (
    AccountObservationRelay, RelayConfigError,
)
from backend.tests.test_observation_read_page import fixture
from tools.orun_observation_readonly_sidecar import make_readonly_server


class ReadonlySidecarTests(unittest.TestCase):
    TOKEN = "SYNTHETIC-SIDECAR-TEST-BEARER-123456"

    def setUp(self) -> None:
        temp = tempfile.TemporaryDirectory(prefix="orun-shadow-test-")
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.db = self.root / "verified.sqlite"
        fixture(self.db, 3)
        self.token_file = self.root / "backend_token"
        self.token_file.write_text(self.TOKEN + "\n", encoding="ascii")
        self.token_file.chmod(0o600)
        self.readonly = make_readonly_server(self.db, self.TOKEN)
        self.serve(self.readonly)

    def serve(self, server: ThreadingHTTPServer) -> None:
        server.daemon_threads = True
        thread = Thread(target=server.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(thread.join, 4)
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)

    @staticmethod
    def request(server, method, route, bearer=None, body=None):
        conn = http.client.HTTPConnection(
            "127.0.0.1", server.server_port, timeout=4)
        headers = {}
        if bearer:
            headers["Authorization"] = "Bearer " + bearer
        try:
            conn.request(method, route, body=body, headers=headers)
            response = conn.getresponse()
            return response.status, json.loads(response.read())
        finally:
            conn.close()

    def test_readonly_only_records_no_mutation_and_bearer_required(self):
        path = "/v1/edge/records?after=0"
        self.assertEqual(self.request(self.readonly, "GET", path)[0], 401)
        self.assertEqual(self.request(
            self.readonly, "GET", path, "wrong-token")[0], 401)
        self.assertEqual(self.request(self.readonly, "GET", "/api/points")[0], 404)
        self.assertEqual(self.request(self.readonly, "GET", "/v1/edge/keys")[0], 404)
        self.assertEqual(self.request(
            self.readonly, "POST", "/v1/edge/batch", self.TOKEN,
            b"{}")[0], 405)
        self.assertEqual(self.request(
            self.readonly, "GET", "/v1/edge/records?after=-1",
            self.TOKEN)[0], 400)
        self.assertEqual(self.request(
            self.readonly, "GET", "/v1/edge/records?after=0&x=1",
            self.TOKEN)[0], 400)
        code, page = self.request(
            self.readonly, "GET", path, self.TOKEN)
        self.assertEqual(code, 200)
        self.assertEqual(page["cursor"], 3)
        self.assertEqual([r["sequence"] for r in page["records"]], [1, 2, 3])
        self.assertEqual(self.request(
            self.readonly, "GET", "/v1/edge/records?after=3",
            self.TOKEN)[1]["records"], [])

    def test_split_relay_preserves_legacy_backend_routes(self):
        expected = self.TOKEN

        class LegacyBackend(BaseHTTPRequestHandler):
            def log_message(self, *_args):
                pass

            def reply(self, status, value):
                raw = json.dumps(value).encode("utf-8")
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(raw)))
                self.end_headers()
                self.wfile.write(raw)

            def authorized(self):
                return self.headers.get("Authorization") == "Bearer " + expected

            def do_GET(self):
                if not self.authorized():
                    return self.reply(401, {"error": "unauthorized"})
                if self.path == "/v1/edge/keys":
                    return self.reply(200, {"keys": []})
                if self.path == "/api/points":
                    return self.reply(200, {"points": [], "count": 0})
                return self.reply(404, {"error": "legacy_has_no_record_feed"})

            def do_POST(self):
                if not self.authorized():
                    return self.reply(401, {"error": "unauthorized"})
                if self.path == "/v1/edge/batch":
                    return self.reply(200, {"records": [], "legacy": True})
                return self.reply(404, {"error": "not_found"})

        legacy = ThreadingHTTPServer(("127.0.0.1", 0), LegacyBackend)
        self.serve(legacy)
        legacy_url = "http://127.0.0.1:" + str(legacy.server_port)
        sidecar_url = "http://127.0.0.1:" + str(self.readonly.server_port)
        relay = AccountObservationRelay(
            legacy_url, self.token_file,
            observation_site="orun-main", records_upstream=sidecar_url,
        )
        status, records = relay.forward("GET", "/v1/edge/records?after=0")
        self.assertEqual(status, 200)
        self.assertEqual(len(records["records"]), 3)
        status, keys = relay.forward("GET", "/v1/edge/keys")
        self.assertEqual(status, 200)
        self.assertEqual(keys["keys"], [])
        status, batch = relay.forward(
            "POST", "/v1/edge/batch",
            b'{"edge":"test","records":[],"sessions":[]}')
        self.assertEqual(status, 200)
        self.assertTrue(batch["legacy"])
        # Legacy map is still served by the old Backend, never sidecar.
        self.assertEqual(relay.host, "127.0.0.1")
        self.assertEqual(relay.port, legacy.server_port)
        self.assertEqual(relay.records_port, self.readonly.server_port)

    def test_bad_split_config_is_rejected(self):
        url = "http://127.0.0.1:8090"
        with self.assertRaises(RelayConfigError):
            AccountObservationRelay(
                url, self.token_file,
                records_upstream="http://127.0.0.1:18783")
        for forbidden in (
            "http://192.168.1.1:8091",
            "http://example.com:8091",
            "https://127.0.0.1:8091",
            "http://127.0.0.1:8091/other",
        ):
            with self.assertRaises(RelayConfigError):
                AccountObservationRelay(
                    url, self.token_file, observation_site="orun-main",
                    records_upstream=forbidden)
        with self.assertRaises(ValueError):
            make_readonly_server(self.db, self.TOKEN, "0.0.0.0", 0)


if __name__ == "__main__":
    unittest.main()
