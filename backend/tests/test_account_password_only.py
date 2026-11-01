"""Password-first ORUN account and authenticated observation relay host tests.

All tests are disposable SQLite + 127.0.0.1 ephemeral HTTP fixtures.
No live backend, systemd, Tailscale, phone, PC Edge or APK is touched.
"""
from __future__ import annotations

import http.client
from http.server import ThreadingHTTPServer
import json
from pathlib import Path
import sqlite3
import tempfile
import threading
import unittest

from backend.account_login import LoginRejected, MAX_LOGIN_FAILURES
from backend.account_login_http import account_handler_factory
from backend.account_observation_relay import (
    AccountObservationRelay, RelayConfigError,
)
from backend.tests.test_account_device_login import (
    AccountHTTPTests, PASSWORD,
)


class PasswordOnlyAccountTests(unittest.TestCase):
    def setUp(self) -> None:
        fixture = AccountHTTPTests(
            methodName="test_owner_login_required_for_phone_shared_data")
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        self.f = fixture

    def owner(self) -> None:
        self.f.accounts.bootstrap_owner(
            "orun-main", "owner", "ownerlogin", PASSWORD)

    def login(self, password: str = PASSWORD, username: str = "ownerlogin"):
        return self.f.request(
            "POST", "/api/v1/account/login", body={
                "username": username, "password": password,
            })

    def test_any_phone_needs_username_and_password_only(self):
        self.owner()  # NO phone key and NO enrollment requests.
        status, login = self.login()
        self.assertEqual(status, 200, login)
        self.assertEqual(login["site"], "orun-main")
        self.assertEqual(login["actor"], "owner")
        self.assertRegex(login["installation"], r"^account-[0-9a-f]{64}$")
        token = login["account_access_token"]
        status, owner = self.f.request("GET", "/api/v1/account/me", token)
        self.assertEqual(status, 200)
        self.assertIn("zone:write", owner["scopes"])
        status, modified = self.f.request(
            "POST", "/api/v1/sync/ops", token, self.f.op("ParolaMera"))
        self.assertEqual(status, 200)
        self.assertEqual(modified["results"][0]["status"], "ACCEPTED")
        status, shared = self.f.request(
            "GET", "/api/v1/sync?cursor=0", token)
        self.assertEqual(status, 200)
        self.assertEqual(shared["changes"][0]["body"]["name"], "ParolaMera")
        self.assertEqual(self.f.request(
            "POST", "/api/v1/account/logout", token)[0], 200)
        self.assertEqual(self.f.request(
            "GET", "/api/v1/account/me", token)[0], 401)

    def test_second_phone_or_factory_reset_just_logs_in(self):
        self.owner()
        status, first = self.login()
        self.assertEqual(status, 200)
        status, second = self.login()
        self.assertEqual(status, 200)
        self.assertNotEqual(
            first["account_access_token"], second["account_access_token"])
        self.assertEqual(first["installation"], second["installation"])
        self.assertEqual(self.f.request("GET", "/api/v1/account/me",
                                        second["account_access_token"])[0], 200)
        # There is no phone registration needed or created.
        with sqlite3.connect(self.f.enrollment.path) as db:
            self.assertEqual(db.execute(
                "SELECT count(*) FROM edge_installs").fetchone()[0], 0)

    def test_password_guessing_is_limited_and_persists(self):
        self.owner()
        for attempt in range(MAX_LOGIN_FAILURES):
            status, _ = self.login(password="incorrect-long-password")
            self.assertEqual(
                status, 429 if attempt == MAX_LOGIN_FAILURES - 1 else 401)
        self.assertEqual(self.login()[0], 429)
        with sqlite3.connect(self.f.accounts_path) as db:
            db.execute("UPDATE account_password_attempts SET locked_until=1")
        self.assertEqual(self.login()[0], 200)

    def test_site_ambiguity_and_other_user_fail_closed(self):
        self.owner()
        self.f.accounts.bootstrap_owner(
            "other-site", "owner", "ownerlogin", "Another-Password-Only-2026!")
        self.assertEqual(self.login()[0], 401)
        self.assertEqual(self.login(username="unknownuser")[0], 401)

    def test_existing_pc_bearer_cannot_use_account_observation_relay(self):
        self.owner()
        pc = self.f.pc()
        status, _ = self.f.request(
            "POST", "/v1/edge/batch", pc, {"edge": "phone-x", "records": []})
        self.assertEqual(status, 401)

    def test_owner_only_proxy_does_not_accept_anonymous_or_pc_bearer(self):
        self.owner()
        _, login = self.login()
        token = login["account_access_token"]
        calls = []

        class FakeRelay:
            def forward(self, method: str, path: str, data: bytes):
                calls.append((method, path, data))
                if method == "GET":
                    return 200, {"keys": []}
                return 200, {"results": [], "sessions_ok": True, "records": 0}

        srv = ThreadingHTTPServer(("127.0.0.1", 0),
            account_handler_factory(
                self.f.store, self.f.enrollment, self.f.accounts,
                observation_relay=FakeRelay()))
        srv.daemon_threads = True
        thread = threading.Thread(target=srv.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(thread.join, 5)
        self.addCleanup(srv.server_close)
        self.addCleanup(srv.shutdown)

        def call(method, path, bearer=None, body=None):
            conn = http.client.HTTPConnection(
                "127.0.0.1", srv.server_port, timeout=5)
            headers = {"Content-Type": "application/json"}
            if bearer:
                headers["Authorization"] = "Bearer " + bearer
            conn.request(
                method, path, body=json.dumps(body).encode() if body else None,
                headers=headers)
            resp = conn.getresponse()
            code = resp.status
            payload = json.loads(resp.read())
            conn.close()
            return code, payload

        self.assertEqual(call("GET", "/v1/edge/keys")[0], 401)
        self.assertEqual(call("GET", "/v1/edge/keys", token)[0], 200)
        self.assertEqual(call(
            "POST", "/v1/edge/batch", token,
            {"edge": "test-phone", "records": [], "sessions": []})[0], 200)
        self.assertEqual([call[0] for call in calls], ["GET", "POST"])

    def test_relay_rejects_public_ip_or_public_token_file(self):
        with tempfile.TemporaryDirectory(prefix="orun-relay-test-") as tmp:
            root = Path(tmp)
            secret = root / "bearer"
            secret.write_text("FAKE-ONLY-SYNTHETIC-SECRET-12345\n")
            secret.chmod(0o600)
            AccountObservationRelay("http://127.0.0.1:8090", secret)
            AccountObservationRelay("http://100.114.36.9:8090", secret)
            with self.assertRaises(RelayConfigError):
                AccountObservationRelay("http://8.8.8.8:8090", secret)
            secret.chmod(0o644)
            with self.assertRaises(RelayConfigError):
                AccountObservationRelay("http://127.0.0.1:8090", secret)


if __name__ == "__main__":
    unittest.main(verbosity=2)
