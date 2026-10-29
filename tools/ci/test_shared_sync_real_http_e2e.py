#!/usr/bin/env python3
"""Real HTTP A -> Python backend -> B/PC interoperability gate.

Runs only on IPv4 loopback and temporary SQLite. Creates genuine Ed25519
per-install enrollments with explicit local owner approval, then runs the
actual TypeScript SharedSyncClient with native Node fetch. NO live Backend,
Tailscale bind, service restart, Gateway/Tracker or Android storage changes.
"""
from __future__ import annotations

import http.client
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
from http.server import ThreadingHTTPServer

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from backend.edge_enrollment import EdgeEnrollment, signed_challenge  # noqa: E402
from backend.shared_sync_http import handler_factory  # noqa: E402
from backend.shared_sync_store import SharedStore  # noqa: E402


def enroll(reg: EdgeEnrollment, installation: str, site: str,
           actor: str, scopes: frozenset[str]) -> str:
    key = Ed25519PrivateKey.generate()
    pub = key.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw)
    pending = reg.begin(installation, pub)
    # Host test only: a trusted caller drives approval, not a REST endpoint.
    code = reg.approve(pending.request_id, pending.fingerprint, site, actor, scopes)
    signature = key.sign(signed_challenge(pending, installation, pub))
    return reg.finish(pending.request_id, installation, pub, signature, code)


def get_status(port: int, token: str) -> int:
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=4)
    try:
        conn.request("GET", "/api/v1/sync?cursor=0",
                     headers={"Authorization": "Bearer " + token})
        response = conn.getresponse()
        response.read()
        return response.status
    finally:
        conn.close()


def main() -> int:
    edit = frozenset({"zone:read", "zone:write", "group:read", "group:write",
                      "zone_target:read", "zone_target:write"})
    view = frozenset({"zone:read", "group:read", "zone_target:read"})
    with tempfile.TemporaryDirectory(prefix="orun-s3c-loopback-") as temp:
        root = Path(temp)
        reg = EdgeEnrollment(root / "edge_registry.sqlite")
        store = SharedStore(root / "shared.sqlite")
        secrets = {
            "A": enroll(reg, "phone-a", "owner-site", "owner", edit),
            "B": enroll(reg, "phone-b", "owner-site", "viewer", view),
            "C": enroll(reg, "phone-c", "other-site", "other-owner", edit),
            "D": enroll(reg, "pc-editor", "owner-site", "owner", edit),
        }
        httpd = ThreadingHTTPServer(("127.0.0.1", 0),
                                     handler_factory(store, reg.resolve_headers))
        httpd.daemon_threads = True
        assert httpd.server_address[0] == "127.0.0.1"
        port = httpd.server_address[1]
        server_thread = threading.Thread(target=httpd.serve_forever, daemon=True)
        server_thread.start()
        try:
            env = dict(os.environ)
            env.update({
                "ORUN_SYNC_E2E_URL": f"http://127.0.0.1:{port}/",
                **{"ORUN_SYNC_E2E_" + k: v for k, v in secrets.items()},
            })
            script = ROOT / "tools" / "ci" / "shared_sync_e2e_client.ts"
            result = subprocess.run(
                ["node", "--experimental-strip-types", str(script)],
                cwd=ROOT, env=env, text=True, capture_output=True, timeout=50,
                check=False,
            )
            if result.returncode:
                print(result.stdout, end="", file=sys.stderr)
                print(result.stderr, end="", file=sys.stderr)
                raise RuntimeError(f"Node real-HTTP integration failed: {result.returncode}")
            print(result.stdout, end="")
            if get_status(port, secrets["A"]) != 200:
                raise AssertionError("valid editor not recognized")
            if not reg.revoke("phone-a"):
                raise AssertionError("owner revocation failed")
            if get_status(port, secrets["A"]) != 401:
                raise AssertionError("revoked installation still authorized")
            if get_status(port, secrets["B"]) != 200:
                raise AssertionError("revoking A also affected B")
            if get_status(port, "invalid-bench-token") != 401:
                raise AssertionError("unenrolled bench credential accepted")
            print("STEP3C_REAL_HTTP_E2E: PASS - authenticated revocation 401; B still authorized")
            print("ISOLATION: IPv4 loopback + temporary SQLite; no running ORUN services changed.")
            return 0
        finally:
            httpd.shutdown()
            httpd.server_close()
            server_thread.join(timeout=5)


if __name__ == "__main__":
    raise SystemExit(main())
