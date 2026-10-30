"""ORUN S6A real HTTP account-login preflight; temporary DBs/localhost only."""
from __future__ import annotations

import base64
import hashlib
import http.client
from http.server import ThreadingHTTPServer
import json
from pathlib import Path
import sqlite3
import tempfile
import threading
import time
import unittest

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives import hashes, serialization

from backend.account_login import (
    AccountError, AccountStore, LoginLocked, LoginRejected, OWNER_SCOPES,
    MAX_LOGIN_FAILURES,
)
from backend.account_login_http import account_handler_factory
from backend.edge_enrollment import EdgeEnrollment, signed_challenge
from backend.shared_sync_store import SharedStore

PASSWORD = "ORUN-Automated-Host-Test-Only-2026!"
RING = [[376100000, 280500000], [376100000, 280510000],
        [376110000, 280510000], [376110000, 280500000]]
READ_ONLY = frozenset(("zone:read", "group:read", "zone_target:read"))


class AccountHTTPTests(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(prefix="orun-s6a-")
        self.addCleanup(tmp.cleanup)
        root = Path(tmp.name)
        self.enrollment = EdgeEnrollment(root / "enrolled.sqlite")
        self.accounts_path = root / "accounts.sqlite"
        self.accounts = AccountStore(self.accounts_path)
        self.store = SharedStore(root / "shared.sqlite")
        self.server = ThreadingHTTPServer(
            ("127.0.0.1", 0),
            account_handler_factory(self.store, self.enrollment, self.accounts))
        self.server.daemon_threads = True
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.addCleanup(self.thread.join, 5)
        self.addCleanup(self.server.server_close)
        self.addCleanup(self.server.shutdown)

    def request(self, method, path, token=None, body=None):
        conn = http.client.HTTPConnection("127.0.0.1",
                                          self.server.server_port, timeout=10)
        headers = {"Content-Type": "application/json"}
        if token:
            headers["Authorization"] = "Bearer " + token
        if body is None:
            raw = None
        elif isinstance(body, bytes):
            raw = body
        else:
            raw = json.dumps(body).encode("utf-8")
        try:
            conn.request(method, path, raw, headers=headers)
            resp = conn.getresponse()
            return resp.status, json.loads(resp.read().decode())
        finally:
            conn.close()

    def phone(self, site="orun-main", actor="owner", scopes=OWNER_SCOPES):
        key = ec.generate_private_key(ec.SECP256R1())
        pub = key.public_key().public_bytes(
            serialization.Encoding.DER,
            serialization.PublicFormat.SubjectPublicKeyInfo)
        installation = "edge-" + hashlib.sha256(pub).hexdigest()
        pending = self.enrollment.begin(
            installation, pub, algorithm="P256-SHA256")
        code = self.enrollment.approve(pending.request_id, pending.fingerprint,
                                       site, actor, scopes)
        sig = key.sign(signed_challenge(
            pending, installation, pub, algorithm="P256-SHA256"),
            ec.ECDSA(hashes.SHA256()))
        token = self.enrollment.finish(
            pending.request_id, installation, pub, sig, code,
            algorithm="P256-SHA256")
        return token, key, pub, installation

    def pc(self):
        key = Ed25519PrivateKey.generate()
        pub = key.public_key().public_bytes(
            serialization.Encoding.Raw, serialization.PublicFormat.Raw)
        pending = self.enrollment.begin("pc-main", pub)
        code = self.enrollment.approve(pending.request_id, pending.fingerprint,
                                       "orun-main", "pc-owner", READ_ONLY)
        return self.enrollment.finish(
            pending.request_id, "pc-main", pub,
            key.sign(signed_challenge(pending, "pc-main", pub)), code)

    def seed(self):
        self.accounts.bootstrap_owner(
            "orun-main", "owner", "ownerlogin", PASSWORD)
        phone_token, key, pub, installation = self.phone()
        return phone_token, key, pub, installation

    def login(self, device, password=PASSWORD, username="ownerlogin"):
        return self.request("POST", "/api/v1/account/login", device, {
            "username": username, "password": password})

    def op(self, name="Mera"):
        return {"ops": [{
            "op_id": "account-" + name, "kind": "zone", "id": "z1",
            "action": "UPSERT", "base_revision": 0,
            "body": {"name": name, "active": True, "ring_e7": RING},
        }]}

    def test_owner_login_required_for_phone_shared_data(self):
        device, _, _, installation = self.seed()
        self.assertEqual(self.request(
            "GET", "/api/v1/sync", device)[0], 401)
        self.assertEqual(self.request(
            "POST", "/api/v1/sync/ops", device, self.op())[0], 401)
        # This one endpoint is for validating the newly enrolled phone
        # installation; it is not access to any synced user data.
        status, ident = self.request(
            "GET", "/api/v1/sync/identity", device)
        self.assertEqual(status, 200)
        self.assertEqual(ident["installation"], installation)
        self.assertEqual(self.request(
            "GET", "/api/v1/account/me", device)[0], 401)
        status, result = self.login(device)
        self.assertEqual(status, 200, result)
        account = result["account_access_token"]
        self.assertNotEqual(account, device)
        self.assertEqual(result["site"], "orun-main")
        self.assertEqual(result["actor"], "owner")
        self.assertEqual(result["installation"], installation)
        status, me = self.request("GET", "/api/v1/account/me", account)
        self.assertEqual(status, 200)
        self.assertEqual(set(me["scopes"]), OWNER_SCOPES)
        status, created = self.request(
            "POST", "/api/v1/sync/ops", account, self.op())
        self.assertEqual(status, 200)
        self.assertEqual(created["results"][0]["status"], "ACCEPTED")
        status, view = self.request(
            "GET", "/api/v1/sync?cursor=0", account)
        self.assertEqual(status, 200)
        self.assertEqual(view["changes"][0]["body"]["name"], "Mera")
        self.assertEqual(self.request("POST", "/api/v1/account/logout",
                                       account)[0], 200)
        self.assertEqual(self.request("GET", "/api/v1/sync", account)[0], 401)

    def test_other_approved_phone_same_owner_sees_shared_records(self):
        device1, _, _, _ = self.seed()
        _, logged1 = self.login(device1)
        t1 = logged1["account_access_token"]
        self.assertEqual(self.request(
            "POST", "/api/v1/sync/ops", t1, self.op("OrtakMera"))[0], 200)
        device2, _, _, _ = self.phone()
        self.assertEqual(self.request(
            "GET", "/api/v1/sync", device2)[0], 401)
        code, logged2 = self.login(device2)
        self.assertEqual(code, 200, logged2)
        t2 = logged2["account_access_token"]
        status, page = self.request("GET", "/api/v1/sync?cursor=0", t2)
        self.assertEqual(status, 200)
        self.assertEqual(page["changes"][0]["body"]["name"], "OrtakMera")

    def test_cross_site_phone_and_account_not_swapped(self):
        a, _, _, _ = self.seed()
        self.accounts.bootstrap_owner("another-site", "owner",
                                      "anotherowner", "Different-Test-Password-2026!")
        b, _, _, _ = self.phone(site="another-site")
        self.assertEqual(self.login(b)[0], 401)
        self.assertEqual(self.login(a, username="anotherowner")[0], 401)
        status, accepted = self.login(
            b, password="Different-Test-Password-2026!",
            username="anotherowner")
        self.assertEqual(status, 200, accepted)
        site_two = accepted["account_access_token"]
        s1, account_one = self.login(a)
        self.assertEqual(s1, 200)
        self.assertEqual(self.request(
            "POST", "/api/v1/sync/ops",
            account_one["account_access_token"], self.op("HiddenMera"))[0], 200)
        status, other = self.request("GET", "/api/v1/sync?cursor=0", site_two)
        self.assertEqual(status, 200)
        self.assertEqual(other["changes"], [])

    def test_pc_mirror_keeps_old_read_only_auth_and_cannot_login(self):
        self.seed()
        pc = self.pc()
        self.assertEqual(self.request(
            "GET", "/api/v1/sync?cursor=0", pc)[0], 200)
        self.assertEqual(self.login(pc)[0], 401)
        status, response = self.request(
            "POST", "/api/v1/sync/ops", pc, self.op())
        self.assertEqual(status, 200)
        self.assertEqual(response["results"][0]["status"], "REJECTED")
        self.assertEqual(self.request(
            "GET", "/api/v1/account/me", pc)[0], 401)

    def test_device_revocation_and_rotation_invalidate_account_session(self):
        device, key, pub, installation = self.seed()
        _, issued = self.login(device)
        account = issued["account_access_token"]
        pending = self.enrollment.begin_recovery(
            installation, pub, algorithm="P256-SHA256")
        code = self.enrollment.approve(
            pending.request_id, pending.fingerprint,
            "orun-main", "owner", OWNER_SCOPES)
        sig = key.sign(signed_challenge(
            pending, installation, pub, algorithm="P256-SHA256"),
            ec.ECDSA(hashes.SHA256()))
        rotated = self.enrollment.finish_recovery(
            pending.request_id, installation, pub, sig, code,
            algorithm="P256-SHA256")
        self.assertIsNone(self.accounts.resolve_session(self.enrollment, account))
        self.assertEqual(self.request("GET", "/api/v1/sync", account)[0], 401)
        self.assertEqual(self.login(device)[0], 401)
        status, new_account = self.login(rotated)
        self.assertEqual(status, 200, new_account)
        self.assertEqual(self.request(
            "GET", "/api/v1/account/me",
            new_account["account_access_token"])[0], 200)
        self.enrollment.revoke(installation)
        self.assertEqual(self.request(
            "GET", "/api/v1/account/me",
            new_account["account_access_token"])[0], 401)

    def test_wrong_login_limited_per_installation_with_persistent_state(self):
        device, _, _, _ = self.seed()
        for i in range(MAX_LOGIN_FAILURES):
            status, _ = self.login(device, password="BAD_PASSWORD")
            self.assertEqual(status, 429 if i == MAX_LOGIN_FAILURES - 1 else 401)
        self.assertEqual(self.login(device)[0], 429)
        reopened = AccountStore(self.accounts_path)
        with self.assertRaises(LoginLocked):
            reopened.login(self.enrollment, device, "ownerlogin", PASSWORD)
        with sqlite3.connect(self.accounts_path) as db:
            db.execute("UPDATE account_attempts SET locked_until=?",
                       (int(time.time()) - 1,))
        self.assertEqual(self.login(device)[0], 200)

    def test_no_public_signup_account_bootstrap_never_overwrites_owner(self):
        self.seed()
        self.assertEqual(self.request(
            "POST", "/api/v1/account/signup", None,
            {"username": "hacker", "password": "hacker-password"})[0], 404)
        with self.assertRaises(AccountError):
            self.accounts.bootstrap_owner(
                "orun-main", "hacker", "badowner", "hacker-password-long")
        self.assertEqual(self.request(
            "POST", "/api/v1/account/login", None,
            {"username": "ownerlogin", "password": PASSWORD})[0], 401)

    def test_unapproved_identity_cannot_login_and_unrecognized_username_no_leak(self):
        self.accounts.bootstrap_owner(
            "orun-main", "owner", "ownerlogin", PASSWORD)
        self.assertEqual(self.request(
            "POST", "/api/v1/account/login", "a" * 64,
            {"username": "ownerlogin", "password": PASSWORD})[0], 401)
        device, _, _, _ = self.phone()
        self.assertEqual(self.login(device, username="otheruser")[0], 401)
        self.assertEqual(self.login(device, password="wrong")[0], 401)
        self.assertEqual(self.request(
            "POST", "/api/v1/account/login", device, b'not json')[0], 400)
        self.assertEqual(self.login(device)[0], 200)


if __name__ == "__main__":
    unittest.main(verbosity=2)
