"""Real HTTP phone enrollment: P-256, legacy Ed25519, local owner-only approval.

All temporary state + localhost sockets. Never launches or changes systemd,
phone APK, Tailscale Serve, Tracker, existing PC shared DB or observation DB.
"""
from __future__ import annotations

import base64
import hashlib
import http.client
from http.server import ThreadingHTTPServer
import json
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
import unittest

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

from backend.edge_enrollment import (
    EdgeEnrollment, EnrollmentRefused, MAX_PENDING_REQUESTS, Pending,
    signed_challenge,
)
from backend.phone_enrollment_http import mobile_handler_factory
from backend.shared_sync_http import handler_factory
from backend.shared_sync_store import SharedStore

VIEW = frozenset({"zone:read", "group:read", "zone_target:read"})
EDIT = frozenset({"zone:read", "zone:write", "group:read", "group:write",
                  "zone_target:read", "zone_target:write"})
RING = [[376100000, 280500000], [376100000, 280510000],
        [376110000, 280510000], [376110000, 280500000]]


def b64(value: bytes) -> str:
    return base64.b64encode(value).decode("ascii")


def new_key(algorithm: str):
    if algorithm == "Ed25519":
        key = Ed25519PrivateKey.generate()
        raw = key.public_key().public_bytes(
            serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    else:
        key = ec.generate_private_key(ec.SECP256R1())
        raw = key.public_key().public_bytes(
            serialization.Encoding.DER,
            serialization.PublicFormat.SubjectPublicKeyInfo)
    return key, raw


def sign(key, pending: Pending, installation: str, public: bytes, algorithm: str):
    proof = signed_challenge(pending, installation, public, algorithm=algorithm)
    if algorithm == "Ed25519":
        return key.sign(proof)
    return key.sign(proof, ec.ECDSA(hashes.SHA256()))


class PhoneEnrollmentHTTPTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix="orun-phone-owner-")
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.registry = EdgeEnrollment(self.root / "enrolled.sqlite")
        self.store = SharedStore(self.root / "shared.sqlite")
        self.srv = ThreadingHTTPServer(("127.0.0.1", 0),
                                       mobile_handler_factory(self.store, self.registry))
        self.srv.daemon_threads = True
        thread = threading.Thread(target=self.srv.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(thread.join, 5)
        self.addCleanup(self.srv.server_close)
        self.addCleanup(self.srv.shutdown)

    def request(self, method: str, path: str, body=None, token=None,
                extra_headers=None):
        conn = http.client.HTTPConnection(
            "127.0.0.1", self.srv.server_port, timeout=5)
        headers = {"Content-Type": "application/json"}
        if token is not None:
            headers["Authorization"] = "Bearer " + token
        if extra_headers:
            headers.update(extra_headers)
        raw = None if body is None else json.dumps(body).encode("utf-8")
        try:
            conn.request(method, path, raw, headers)
            response = conn.getresponse()
            return response.status, json.loads(response.read())
        finally:
            conn.close()

    def begin(self, algorithm="P256-SHA256"):
        key, public = new_key(algorithm)
        body = {"algorithm": algorithm, "public_key_base64": b64(public)}
        status, data = self.request("POST", "/api/v1/enroll/begin", body)
        self.assertEqual(status, 200, data)
        self.assertEqual(data["algorithm"], algorithm)
        self.assertEqual(data["installation_id"],
                         "edge-" + hashlib.sha256(public).hexdigest())
        self.assertEqual(data["fingerprint"],
                         hashlib.sha256(public).hexdigest().upper()[:20])
        self.assertEqual(len(base64.b64decode(data["challenge_base64"])), 32)
        return key, public, data

    def finish(self, key, public, data, code, *, algorithm="P256-SHA256",
               signature=None, request_id=None):
        pending = Pending(data["request_id"],
                          base64.b64decode(data["challenge_base64"]),
                          data["fingerprint"], data["expires_at"])
        sig = sign(key, pending, data["installation_id"], public, algorithm) if (
            signature is None) else signature
        return self.request("POST", "/api/v1/enroll/finish", {
            "algorithm": algorithm,
            "public_key_base64": b64(public),
            "request_id": request_id or data["request_id"],
            "signature_base64": b64(sig),
            "owner_code": code,
        })

    def test_p256_owner_approved_http_registration_then_scoped_shared_sync(self):
        key, public, pending = self.begin()
        denied, _ = self.finish(key, public, pending, "x" * 43)
        self.assertEqual(denied, 403)
        self.assertEqual(self.request(
            "GET", "/api/v1/sync/identity",
            extra_headers={"X-Site-ID": "orun-main", "X-Actor": "owner",
                           "X-Scopes": "zone:write"})[0], 401)
        code = self.registry.approve(
            pending["request_id"], pending["fingerprint"],
            "orun-main", "owner", EDIT)
        status, issued = self.finish(key, public, pending, code)
        self.assertEqual(status, 200, issued)
        token = issued["access_token"]
        self.assertGreaterEqual(len(token), 48)
        self.assertNotEqual(token, code)
        self.assertEqual(self.request(
            "GET", "/api/v1/sync/identity", token=token),
            (200, {"site": "orun-main", "installation": pending["installation_id"]}))
        record = {"op_id": "phone-zone-1", "kind": "zone", "id": "z1",
                  "action": "UPSERT", "base_revision": 0,
                  "body": {"name": "Telefon merasi", "active": True, "ring_e7": RING}}
        status, response = self.request(
            "POST", "/api/v1/sync/ops", {"ops": [record]}, token)
        self.assertEqual(status, 200)
        self.assertEqual(response["results"][0]["status"], "ACCEPTED")
        status, records = self.request(
            "GET", "/api/v1/sync?cursor=0", token=token)
        self.assertEqual(status, 200)
        self.assertEqual(records["changes"][0]["body"]["name"], "Telefon merasi")
        self.assertEqual(self.finish(key, public, pending, code)[0], 403)

    def test_p256_wrong_proof_algorithm_and_key_are_rejected(self):
        key, public, pending = self.begin()
        code = self.registry.approve(
            pending["request_id"], pending["fingerprint"],
            "orun-main", "owner", VIEW)
        impostor, _ = new_key("P256-SHA256")
        wrong = impostor.sign(b"not-the-pending-challenge",
                              ec.ECDSA(hashes.SHA256()))
        self.assertEqual(self.finish(key, public, pending, code, signature=wrong)[0], 403)
        self.assertEqual(self.finish(key, public, pending, code,
                                     request_id="f" * 32)[0], 403)
        self.assertEqual(self.finish(key, public, pending, code,
                                     algorithm="Ed25519")[0], 400)
        self.assertEqual(self.finish(key, public, pending, code)[0], 200)

    def test_ed25519_original_wire_compatibility_and_read_only(self):
        key, pub, pending = self.begin("Ed25519")
        code = self.registry.approve(
            pending["request_id"], pending["fingerprint"],
            "orun-main", "viewer", VIEW)
        status, issued = self.finish(key, pub, pending, code,
                                     algorithm="Ed25519")
        self.assertEqual(status, 200)
        op = {"op_id": "denied", "kind": "zone", "id": "z1",
              "action": "UPSERT", "base_revision": 0,
              "body": {"name": "Yok", "active": True, "ring_e7": RING}}
        status, data = self.request("POST", "/api/v1/sync/ops",
                                    {"ops": [op]}, token=issued["access_token"])
        self.assertEqual(status, 200)
        self.assertEqual(data["results"][0]["status"], "REJECTED")
        self.assertTrue(self.registry.revoke(pending["installation_id"]))
        self.assertEqual(self.request(
            "GET", "/api/v1/sync/identity",
            token=issued["access_token"])[0], 401)

    def test_public_approval_endpoint_does_not_exist_and_begin_is_opt_in(self):
        key, pub = new_key("P256-SHA256")
        request = {"algorithm": "P256-SHA256", "public_key_base64": b64(pub)}
        self.assertEqual(self.request(
            "POST", "/api/v1/enroll/approve", request)[0], 404)
        bare = ThreadingHTTPServer(("127.0.0.1", 0), handler_factory(
            self.store, self.registry.resolve_headers))
        bare.daemon_threads = True
        worker = threading.Thread(target=bare.serve_forever, daemon=True)
        worker.start()
        try:
            conn = http.client.HTTPConnection(
                "127.0.0.1", bare.server_port, timeout=5)
            conn.request("POST", "/api/v1/enroll/begin",
                         json.dumps(request), {"Content-Type": "application/json"})
            self.assertEqual(conn.getresponse().status, 404)
            conn.close()
        finally:
            bare.shutdown()
            bare.server_close()
            worker.join(timeout=5)

    def test_noncanonical_base64_wrong_curve_malformed_json_denied(self):
        key, pub = new_key("P256-SHA256")
        bad = {"algorithm": "P256-SHA256", "public_key_base64": b64(pub) + "="}
        self.assertEqual(self.request("POST", "/api/v1/enroll/begin", bad)[0], 400)
        secp384 = ec.generate_private_key(ec.SECP384R1()).public_key().public_bytes(
            serialization.Encoding.DER,
            serialization.PublicFormat.SubjectPublicKeyInfo)
        bad["public_key_base64"] = b64(secp384)
        self.assertEqual(self.request("POST", "/api/v1/enroll/begin", bad)[0], 400)
        self.assertEqual(self.request("POST", "/api/v1/enroll/begin?admin=true",
                                      bad)[0], 404)
        self.assertEqual(self.request("POST", "/api/v1/enroll/begin",
                                      {"algorithm": "P256-SHA256"})[0], 400)

    def test_pending_capacity_and_approved_request_protection(self):
        key, pub, original = self.begin()
        self.registry.approve(original["request_id"], original["fingerprint"],
                              "orun-main", "owner", VIEW)
        self.assertEqual(self.request("POST", "/api/v1/enroll/begin", {
            "algorithm": "P256-SHA256", "public_key_base64": b64(pub)})[0], 403)
        for _ in range(MAX_PENDING_REQUESTS - 1):
            self.begin()
        extra, extra_pub = new_key("P256-SHA256")
        self.assertEqual(self.request("POST", "/api/v1/enroll/begin", {
            "algorithm": "P256-SHA256", "public_key_base64": b64(extra_pub)})[0], 429)

    def test_legacy_ed25519_database_schema_migration_no_token_loss(self):
        old = self.root / "legacy.sqlite"
        with sqlite3.connect(old) as db:
            db.execute("CREATE TABLE edge_pending(request_id TEXT PRIMARY KEY,"
                       "installation_id TEXT NOT NULL UNIQUE,public_key BLOB NOT NULL,"
                       "challenge BLOB NOT NULL,expires_at INTEGER NOT NULL,"
                       "owner_code_hash BLOB,site_id TEXT,actor_id TEXT,scopes_json TEXT)")
            db.execute("CREATE TABLE edge_installs(installation_id TEXT PRIMARY KEY,"
                       "public_key BLOB NOT NULL,site_id TEXT NOT NULL,"
                       "actor_id TEXT NOT NULL,scopes_json TEXT NOT NULL,"
                       "token_hash BLOB NOT NULL UNIQUE,expires_at INTEGER NOT NULL,"
                       "revoked_at INTEGER)")
            original_token = "t" * 64
            db.execute("INSERT INTO edge_installs VALUES(?,?,?,?,?,?,?,NULL)",
                       ("pc-existing", b"X" * 32, "orun-main", "owner",
                        json.dumps(sorted(VIEW)),
                        hashlib.sha256(original_token.encode()).digest(),
                        int(time.time()) + 3600))
        old.chmod(0o600)
        migrated = EdgeEnrollment(old)
        found = migrated.resolve_token(original_token)
        self.assertIsNotNone(found)
        self.assertEqual(found.installation_id, "pc-existing")
        self.assertEqual(found.scopes, VIEW)
        with sqlite3.connect(old) as db:
            self.assertEqual(db.execute(
                "SELECT key_algorithm FROM edge_installs").fetchone()[0], "Ed25519")


if __name__ == "__main__":
    unittest.main(verbosity=2)
