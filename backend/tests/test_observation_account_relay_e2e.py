"""Disposable HTTP E2E: account -> private relay -> verified-record Backend.

No live service, credential DB, network interface or tracker is touched.
The test uses synthetic frame BYTES to establish transport/authorization only;
the separate Android AEAD tests establish frame integrity verification.
"""
from __future__ import annotations

import http.client
import json
import os
from http.server import ThreadingHTTPServer
from pathlib import Path
from types import SimpleNamespace
import tempfile
import threading
import unittest

from backend.account_login_http import account_handler_factory
from backend.account_observation_relay import AccountObservationRelay
from backend.orun_backend_bench import make_server
from backend.tests.test_account_device_login import AccountHTTPTests, PASSWORD
from backend.tests.test_observation_read_page import fixture


class AccountObservationRouteE2ETests(unittest.TestCase):
    def setUp(self) -> None:
        base = AccountHTTPTests(
            methodName="test_owner_login_required_for_phone_shared_data")
        base.setUp()
        self.addCleanup(base.doCleanups)
        base.accounts.bootstrap_owner(
            "orun-main", "owner", "ownerlogin", PASSWORD)
        self.base = base

        work = tempfile.TemporaryDirectory(prefix="orun-records-e2e-")
        self.addCleanup(work.cleanup)
        root = Path(work.name)
        db = root / "verified.sqlite"
        fixture(db, 2)
        token = "SYNTHETIC-ACCOUNT-RELAY-TEST-TOKEN"
        bearer_file = root / "upstream_token"
        bearer_file.write_text(token + "\n", encoding="ascii")
        bearer_file.chmod(0o600)

        backend = make_server(
            SimpleNamespace(db_path=db), "127.0.0.1", 0, token)
        self.serve(backend)
        self.upstream = (
            "http://127.0.0.1:" + str(backend.server_port))
        self.token_file = bearer_file

        relay = AccountObservationRelay(
            self.upstream, bearer_file, observation_site="orun-main")
        self.account_server = self.account_http(relay)

    def serve(self, server: ThreadingHTTPServer) -> None:
        server.daemon_threads = True
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(thread.join, 5)
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)

    def account_http(self, relay: AccountObservationRelay) -> ThreadingHTTPServer:
        server = ThreadingHTTPServer(
            ("127.0.0.1", 0),
            account_handler_factory(
                self.base.store, self.base.enrollment,
                self.base.accounts, observation_relay=relay),
        )
        self.serve(server)
        return server

    def request(self, server: ThreadingHTTPServer, method: str,
                route: str, bearer: str | None = None,
                body: dict | None = None) -> tuple[int, dict]:
        conn = http.client.HTTPConnection(
            "127.0.0.1", server.server_port, timeout=5)
        headers = {"Accept": "application/json"}
        if bearer:
            headers["Authorization"] = "Bearer " + bearer
        payload = None
        if body is not None:
            headers["Content-Type"] = "application/json"
            payload = json.dumps(body).encode("utf-8")
        try:
            conn.request(method, route, body=payload, headers=headers)
            reply = conn.getresponse()
            return reply.status, json.loads(reply.read())
        finally:
            conn.close()

    def signed_in(self) -> str:
        status, response = self.request(
            self.account_server, "POST", "/api/v1/account/login",
            body={"username": "ownerlogin", "password": PASSWORD})
        self.assertEqual(status, 200)
        return response["account_access_token"]

    def test_account_to_backend_pages_without_leaking_upstream_bearer(self):
        path = "/v1/edge/records?after=0"
        self.assertEqual(
            self.request(self.account_server, "GET", path)[0], 401)
        self.assertEqual(
            self.request(self.account_server, "GET", path,
                         bearer="not-an-account")[0], 401)
        # Private backend is NOT an uncredentialed account API.
        self.assertEqual(self.request(
            self.account_server, "GET", "/api/v1/account/me")[0], 401)
        account_token = self.signed_in()
        status, page = self.request(
            self.account_server, "GET", path, account_token)
        self.assertEqual(status, 200)
        self.assertEqual(page["cursor"], 2)
        self.assertFalse(page["has_more"])
        self.assertEqual(
            [r["sequence"] for r in page["records"]], [1, 2])
        self.assertEqual(
            self.request(self.account_server, "GET",
                         "/v1/edge/records?after=2",
                         account_token)[1]["records"], [])
        self.assertEqual(
            self.request(self.account_server, "GET",
                         "/v1/edge/records?after=-1",
                         account_token)[0], 503)
        self.assertEqual(
            self.request(self.account_server, "GET",
                         "/v1/edge/records?after=0&x=1",
                         account_token)[0], 503)

    def test_wrong_site_is_denied_at_account_boundary(self):
        token = self.signed_in()
        relay = AccountObservationRelay(
            self.upstream, self.token_file, observation_site="other-site")
        wrong_site_server = self.account_http(relay)
        status, body = self.request(
            wrong_site_server, "GET", "/v1/edge/records?after=0", token)
        self.assertEqual(status, 403, body)
        self.assertEqual(body["error"], "observation_site_not_authorized")
        # A site mismatch must not expose traffic read keys either.
        self.assertEqual(self.request(
            wrong_site_server, "GET", "/v1/edge/keys", token)[0], 403)
        # Nor may it use this other site's Backend to upload frames.
        self.assertEqual(self.request(
            wrong_site_server, "POST", "/v1/edge/batch", token,
            body={"edge": "test-phone", "records": [], "sessions": []})[0], 403)

    def test_live_db_readonly_account_relay(self):
        """Opt-in only: real verified ledger with synthetic local credentials.

        The Backend and account HTTP listeners are both ephemeral loopback.
        Account/password DB and bearer are disposable; the real ledger is
        accessed only by the existing sqlite mode=ro observation reader.
        """
        live_path = os.environ.get("ORUN_OBSERVATION_READONLY_DB")
        if not live_path:
            self.skipTest("ORUN_OBSERVATION_READONLY_DB not explicitly supplied")
        live_db = Path(live_path).expanduser()
        self.assertTrue(live_db.is_file(), "Specified read-only ledger missing")

        # Separate from the fake fixture and the already-running Backend.
        backend = make_server(
            SimpleNamespace(db_path=live_db), "127.0.0.1", 0,
            "SYNTHETIC-ACCOUNT-RELAY-TEST-TOKEN",
        )
        self.serve(backend)
        upstream = "http://127.0.0.1:" + str(backend.server_port)
        relay = AccountObservationRelay(
            upstream, self.token_file, observation_site="orun-main",
        )
        account = self.account_http(relay)
        path = "/v1/edge/records?after=0"
        # Account credentials are from a temporary fixture, not the live site.
        self.assertEqual(self.request(account, "GET", path)[0], 401)
        access = self.signed_in()
        status, first = self.request(account, "GET", path, access)
        self.assertEqual(status, 200)
        self.assertEqual(len(first["records"]), 64)
        self.assertEqual(first["cursor"], 64)
        self.assertTrue(first["has_more"])
        self.assertTrue(all(
            isinstance(record["frame"], str)
            and len(record["frame"]) in (168, 200)
            for record in first["records"]
        ))
        second_status, second = self.request(
            account, "GET", "/v1/edge/records?after=64", access
        )
        self.assertEqual(second_status, 200)
        self.assertGreater(len(second["records"]), 0)
        self.assertGreater(second["cursor"], first["cursor"])
        self.assertNotEqual(first["records"][0]["cursor"],
                            second["records"][0]["cursor"])

    def test_no_operator_site_opt_in_refuses_read(self):
        token = self.signed_in()
        relay = AccountObservationRelay(self.upstream, self.token_file)
        disabled_server = self.account_http(relay)
        status, body = self.request(
            disabled_server, "GET", "/v1/edge/records?after=0", token)
        self.assertEqual(status, 403, body)


if __name__ == "__main__":
    unittest.main()
