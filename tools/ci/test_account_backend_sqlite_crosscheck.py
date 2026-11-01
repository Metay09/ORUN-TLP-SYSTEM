#!/usr/bin/env python3
"""S6E isolated Python Backend → real HTTP → TypeScript → node:sqlite.

Backend is bound ONLY to 127.0.0.1 on an ephemeral port. Credentials are
generated in temporary memory and passed via child's stdin (never argv, env,
URL, logs or a persistent file). Nothing touches the live ORUN runtime.
Node uses a TEST-ONLY URL adapter to forward a synthetic HTTPS origin to
localhost HTTP; production OwnerAccountClient still requires real HTTPS.
"""
from __future__ import annotations

import hashlib
from http.server import ThreadingHTTPServer
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import threading

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec

from backend.account_login import AccountStore, OWNER_SCOPES
from backend.account_login_http import account_handler_factory
from backend.edge_enrollment import EdgeEnrollment, signed_challenge
from backend.shared_sync_store import Actor, SharedStore

PASSWORD = "Synthetic-E2E-Host-Only-2026!DoNotReuse"
RING = [
    [376100000, 280500000], [376100000, 280510000],
    [376110000, 280510000], [376110000, 280500000],
]


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="orun-s6e-e2e-") as directory:
        root = Path(directory)
        enrolled = EdgeEnrollment(root / "enrolled.sqlite")
        accounts = AccountStore(root / "accounts.sqlite")
        store = SharedStore(root / "shared.sqlite")
        accounts.bootstrap_owner("orun-main", "owner", "ownerlogin", PASSWORD)

        private_key = ec.generate_private_key(ec.SECP256R1())
        public = private_key.public_key().public_bytes(
            serialization.Encoding.DER,
            serialization.PublicFormat.SubjectPublicKeyInfo,
        )
        installation = "edge-" + hashlib.sha256(public).hexdigest()
        pending = enrolled.begin(
            installation, public, algorithm="P256-SHA256")
        owner_code = enrolled.approve(
            pending.request_id, pending.fingerprint, "orun-main", "owner",
            OWNER_SCOPES)
        proof = signed_challenge(
            pending, installation, public, algorithm="P256-SHA256")
        signature = private_key.sign(proof, ec.ECDSA(hashes.SHA256()))
        device_token = enrolled.finish(
            pending.request_id, installation, public,
            signature, owner_code, algorithm="P256-SHA256")

        # The fixture is a trusted owner-side seeding action. The HTTP API
        # never exposes a public initial-data bypass.
        actor = Actor("orun-main", "owner", installation, OWNER_SCOPES)
        operations = [
            {"op_id": "fixture-zone", "kind": "zone", "id": "z1",
             "action": "UPSERT", "base_revision": 0,
             "body": {"name": "Kuzey", "active": True, "ring_e7": RING}},
            {"op_id": "fixture-group", "kind": "group", "id": "g1",
             "action": "UPSERT", "base_revision": 0,
             "body": {"name": "Birinci", "device_ids": []}},
            {"op_id": "fixture-target", "kind": "zone_target", "id": "z1",
             "action": "UPSERT", "base_revision": 0,
             "body": {"zone_id": "z1", "scope": "all",
                      "device_ids": [], "group_ids": []}},
        ]
        seeded = store.mutate(actor, operations)
        if len(seeded) != 3 or any(
                item.get("status") != "ACCEPTED" for item in seeded):
            raise RuntimeError("fixture seed rejected")

        httpd = ThreadingHTTPServer(
            ("127.0.0.1", 0),
            account_handler_factory(store, enrolled, accounts))
        httpd.daemon_threads = True
        thread = threading.Thread(target=httpd.serve_forever, daemon=True)
        thread.start()
        try:
            fixture = json.dumps({
                "port": httpd.server_port,
                "site": "orun-main",
                "installation": installation,
                "deviceToken": device_token,
                "password": PASSWORD,
            })
            result = subprocess.run(
                ["node", "--experimental-strip-types",
                 "tools/ci/test_account_backend_sqlite_crosscheck.ts"],
                cwd=ROOT, input=fixture, text=True,
                capture_output=True, timeout=60, check=False)
            # No credentials may appear in the pass/failure output.
            combined = (result.stdout + result.stderr)
            for secret in (device_token, PASSWORD, owner_code):
                combined = combined.replace(secret, "[REDACTED]")
            if result.returncode:
                print(combined[:5000])
                raise RuntimeError("S6E cross-language E2E test failed")
            print(combined.strip())
            print("ORUN_ACCOUNT_REAL_HTTP_SQLITE_E2E_PASS")
        finally:
            httpd.shutdown()
            httpd.server_close()
            thread.join(timeout=5)


if __name__ == "__main__":
    main()
