"""S6H: exercise the pinned S4E legacy PC credential schema under S6A login.

Only synthetic tokens and temporary 0700 SQLite, localhost port 0.
Never reads the user's actual S4E state or changes systemd/Tailscale/APK.
This is NOT a live migration test or production authorization.
"""
from __future__ import annotations

import hashlib
from http.server import ThreadingHTTPServer
import http.client
import json
from pathlib import Path
import sqlite3
import tempfile
import threading
import time
import unittest

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

from backend.account_login import AccountStore
from backend.account_login_http import account_handler_factory
from backend.edge_enrollment import EdgeEnrollment, signed_challenge
from backend.shared_sync_store import Actor, SharedStore

SITE = "orun-main"
PC_INSTALLATION = "pc-main"
PASSWORD = "S6H-Synthetic-Owner-Password-Do-Not-Reuse!"
PC_TOKEN = "S6H" + "p" * 61
READ = frozenset({"zone:read", "group:read", "zone_target:read"})
EDIT = frozenset({"zone:read", "zone:write", "group:read", "group:write",
                  "zone_target:read", "zone_target:write"})
RING = [[376100000, 280500000], [376100000, 280510000],
        [376110000, 280510000], [376110000, 280500000]]


def make_legacy_pc_registry(path: Path) -> bytes:
    """Exactly the pre-P256 column layout at pinned S4E 264b4117."""
    key = Ed25519PrivateKey.generate()
    public = key.public_key().public_bytes(
        serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    with sqlite3.connect(path) as db:
        db.executescript("""
            CREATE TABLE edge_pending (
                request_id TEXT PRIMARY KEY,
                installation_id TEXT NOT NULL UNIQUE,
                public_key BLOB NOT NULL,
                challenge BLOB NOT NULL,
                expires_at INTEGER NOT NULL,
                owner_code_hash BLOB,
                site_id TEXT,
                actor_id TEXT,
                scopes_json TEXT);
            CREATE TABLE edge_installs (
                installation_id TEXT PRIMARY KEY,
                public_key BLOB NOT NULL,
                site_id TEXT NOT NULL,
                actor_id TEXT NOT NULL,
                scopes_json TEXT NOT NULL,
                token_hash BLOB NOT NULL UNIQUE,
                expires_at INTEGER NOT NULL,
                revoked_at INTEGER);
            CREATE INDEX edge_token_live
              ON edge_installs(token_hash,revoked_at);
        """)
        db.execute(
            "INSERT INTO edge_installs "
            "(installation_id,public_key,site_id,actor_id,scopes_json,"
            "token_hash,expires_at,revoked_at) VALUES(?,?,?,?,?,?,?,NULL)",
            (PC_INSTALLATION, public, SITE, "owner",
             json.dumps(sorted(READ)),
             hashlib.sha256(PC_TOKEN.encode("ascii")).digest(),
             int(time.time()) + 3600),
        )
    path.chmod(0o600)
    return public


class S6HLegacyPcUnderAccountMode(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix="orun-s6h-shadow-")
        self.addCleanup(temp.cleanup)
        base = Path(temp.name)
        self.legacy_path = base / "enrolled_edges.sqlite"
        self.original_pc_public = make_legacy_pc_registry(self.legacy_path)
        self.original_pc_token_hash = hashlib.sha256(
            PC_TOKEN.encode("ascii")).digest()

        self.registry = EdgeEnrollment(self.legacy_path)
        self.shared = SharedStore(base / "shared_resources.sqlite")
        self.accounts = AccountStore(base / "owner_accounts.sqlite")
        self.accounts.bootstrap_owner(SITE, "owner", "ownerlogin", PASSWORD)

        seeded = self.shared.mutate(
            Actor(SITE, "owner", "fixture-authority", EDIT),
            [{
                "op_id": "s6h-fixture-zone",
                "kind": "zone",
                "id": "legacy-zone",
                "action": "UPSERT",
                "base_revision": 0,
                "body": {"name": "Eski sistem merasi",
                         "active": True, "ring_e7": RING},
            }],
        )
        self.assertEqual(len(seeded), 1)
        self.assertEqual(seeded[0]["status"], "ACCEPTED")

        server = ThreadingHTTPServer(
            ("127.0.0.1", 0),
            account_handler_factory(self.shared, self.registry,
                                    self.accounts,
                                    enable_phone_enrollment=False),
        )
        server.daemon_threads = True
        self.server = server
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        self.addCleanup(worker.join, 5)
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)

    def request(self, method: str, endpoint: str,
                bearer: str | None = None,
                body: dict | None = None) -> tuple[int, dict]:
        # Localhost-only; never prints any bearer token or password.
        connection = http.client.HTTPConnection(
            "127.0.0.1", self.server.server_port, timeout=5)
        headers = {"Accept": "application/json"}
        if bearer is not None:
            headers["Authorization"] = "Bearer " + bearer
        data = None
        if body is not None:
            data = json.dumps(body).encode("utf-8")
            headers["Content-Type"] = "application/json"
        try:
            connection.request(method, endpoint, data, headers)
            response = connection.getresponse()
            return response.status, json.loads(response.read())
        finally:
            connection.close()

    def enroll_synthetic_p256(self) -> str:
        private = ec.generate_private_key(ec.SECP256R1())
        public = private.public_key().public_bytes(
            serialization.Encoding.DER,
            serialization.PublicFormat.SubjectPublicKeyInfo,
        )
        installation = "edge-" + hashlib.sha256(public).hexdigest()
        pending = self.registry.begin(
            installation, public, algorithm="P256-SHA256")
        owner_code = self.registry.approve(
            pending.request_id, pending.fingerprint,
            SITE, "owner", EDIT,
        )
        signed = signed_challenge(
            pending, installation, public, algorithm="P256-SHA256")
        signature = private.sign(signed, ec.ECDSA(hashes.SHA256()))
        return self.registry.finish(
            pending.request_id, installation, public,
            signature, owner_code, algorithm="P256-SHA256",
        )

    def test_legacy_pc_remains_read_only_under_account_login(self):
        self.assertEqual(
            self.request("GET", "/api/v1/sync/identity", PC_TOKEN),
            (200, {"site": SITE, "installation": PC_INSTALLATION}),
        )
        status, result = self.request(
            "GET", "/api/v1/sync?cursor=0", PC_TOKEN)
        self.assertEqual(status, 200)
        self.assertEqual(result["changes"][0]["body"]["name"],
                         "Eski sistem merasi")
        self.assertEqual(self.request(
            "POST", "/api/v1/account/login", PC_TOKEN,
            {"username": "ownerlogin", "password": PASSWORD})[0], 401)
        status, result = self.request(
            "POST", "/api/v1/sync/ops", PC_TOKEN,
            {"ops": [{
                "op_id": "denied-old-pc-write", "kind": "zone", "id": "z2",
                "action": "UPSERT", "base_revision": 0,
                "body": {"name": "Denied", "active": True,
                         "ring_e7": RING},
            }]},
        )
        self.assertEqual(status, 200)
        self.assertEqual(result["results"][0]["status"], "REJECTED")
        self.assertEqual(len(self.shared.pull(
            Actor(SITE, "owner", PC_INSTALLATION, READ), 0)["changes"]), 1)

    def test_new_phone_must_log_in_and_pc_still_works_after_logout(self):
        phone = self.enroll_synthetic_p256()
        self.assertEqual(
            self.request("GET", "/api/v1/sync?cursor=0", phone)[0], 401)
        self.assertEqual(self.request(
            "GET", "/api/v1/sync/identity", phone)[0], 200)
        status, result = self.request(
            "POST", "/api/v1/account/login", phone,
            {"username": "ownerlogin", "password": PASSWORD})
        self.assertEqual(status, 200)
        account_token = result["account_access_token"]
        self.assertEqual(
            self.request("GET", "/api/v1/sync?cursor=0",
                         account_token)[0], 200)
        self.assertEqual(
            self.request("POST", "/api/v1/account/logout",
                         account_token)[0], 200)
        self.assertEqual(
            self.request("GET", "/api/v1/sync?cursor=0",
                         account_token)[0], 401)
        self.assertEqual(
            self.request("GET", "/api/v1/sync?cursor=0", PC_TOKEN)[0], 200)

    def test_legacy_token_and_original_fields_survive_additive_migration(self):
        with sqlite3.connect(self.legacy_path) as db:
            before = db.execute(
                "SELECT public_key,site_id,actor_id,scopes_json,token_hash,"
                "expires_at,revoked_at FROM edge_installs "
                "WHERE installation_id=?", (PC_INSTALLATION,),
            ).fetchone()
            self.assertEqual(before[0], self.original_pc_public)
            self.assertEqual(before[1:3], (SITE, "owner"))
            self.assertEqual(before[4], self.original_pc_token_hash)
            self.assertIsNone(before[6])
            columns = {item[1] for item in db.execute(
                "PRAGMA table_info(edge_installs)")}
            self.assertIn("key_algorithm", columns)
            self.assertEqual(db.execute(
                "SELECT key_algorithm FROM edge_installs WHERE "
                "installation_id=?", (PC_INSTALLATION,),
            ).fetchone()[0], "Ed25519")
            self.assertIn("purpose", {item[1] for item in db.execute(
                "PRAGMA table_info(edge_pending)")})
        # Old schema's original SELECT shape still works; this does not
        # prove a complete live rollback, which requires a pinned backup.
        self.assertEqual(
            self.registry.resolve_token(PC_TOKEN).installation_id,
            PC_INSTALLATION,
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
