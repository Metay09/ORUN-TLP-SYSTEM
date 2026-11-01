#!/usr/bin/env python3
"""S4D isolated nonempty shared-sync HTTP proof alongside *running* ORUN.

This is test-only. It refuses to work outside an S4D named temporary
directory. Uses owner-authorized Step3C Ed25519 to write E7 zone/group/target
via authenticated HTTP, and confirms read-only PC mirror plus tombstones.
Never addresses the user's observation backend, PC Edge observation DB, real
device firmware, phone or any non-test token.
"""
from __future__ import annotations

import argparse
import http.client
import json
import os
from pathlib import Path
import stat
import sys

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from backend.edge_enrollment import EdgeEnrollment, signed_challenge  # noqa: E402
from tools.orun_shared_sync_loopback import FILES  # noqa: E402
from tools.orun_shared_pc_mirror import PcSharedMirror  # noqa: E402

SITE = "integration-demo"
INSTALL = "phone-shadow-writer"
TOKEN = "shadow-writer.token"
ZONE = {"name": "Test Merasi", "active": True, "ring_e7": [
    [376100000, 280540000], [376100000, 280550000],
    [376110000, 280550000], [376110000, 280540000]]}
GROUP = {"name": "Test Surusu", "device_ids": ["T-01"]}
TARGET = {"zone_id": "shadow-zone", "scope": "selected",
          "device_ids": ["T-01"], "group_ids": []}


def check_work(work: Path) -> None:
    real = work.resolve(strict=True)
    # A private, temporary test root created by run_shared_shadow_on_existing_pc.sh.
    if (real.name != work.name or not real.name.startswith("orun-shared-s4d.") or
            not real.is_dir() or real.is_symlink() or
            real.stat().st_uid != os.geteuid() or
            stat.S_IMODE(real.stat().st_mode) & 0o077 or
            not (real / "backend").is_dir() or
            not (real / "secrets").is_dir()):
        raise ValueError("SHADOW_TEMP_STATE_REQUIRED")
    for dirpath in (real / "backend", real / "secrets"):
        if dirpath.is_symlink() or stat.S_IMODE(dirpath.stat().st_mode) & 0o077:
            raise ValueError("SHADOW_PRIVATE_DIRECTORY_REQUIRED")
    for name in FILES:
        path = real / "backend" / name
        if (path.is_symlink() or not path.is_file() or
                stat.S_IMODE(path.stat().st_mode) & 0o077):
            raise ValueError("SHADOW_DB_PRIVATE_PAIR_REQUIRED")


def enroll_writer(work: Path) -> str:
    registry = EdgeEnrollment(work / "backend" / FILES[1])
    private = Ed25519PrivateKey.generate()
    pub = private.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw)
    request = registry.begin(INSTALL, pub)
    scopes = frozenset({"zone:read", "zone:write", "group:read", "group:write",
                        "zone_target:read", "zone_target:write"})
    code = registry.approve(request.request_id, request.fingerprint,
                            SITE, "shadow-owner", scopes)
    token = registry.finish(request.request_id, INSTALL, pub,
                            private.sign(signed_challenge(request, INSTALL, pub)), code)
    file = work / "secrets" / TOKEN
    with os.fdopen(os.open(file, os.O_WRONLY | os.O_CREAT | os.O_EXCL |
                          getattr(os, "O_NOFOLLOW", 0), 0o600), "wb") as f:
        f.write((token + "\n").encode("ascii"))
        f.flush()
        os.fsync(f.fileno())
    return token


def read_token(work: Path) -> str:
    file = work / "secrets" / TOKEN
    if file.is_symlink() or stat.S_IMODE(file.stat().st_mode) & 0o077:
        raise ValueError("SHADOW_WRITER_TOKEN_NOT_PRIVATE")
    return file.read_text(encoding="ascii").strip()


def mutate(port: int, token: str, ops: list[dict]) -> None:
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=8)
    try:
        conn.request("POST", "/api/v1/sync/ops",
                     body=json.dumps({"ops": ops}).encode("utf-8"),
                     headers={"Authorization": "Bearer " + token,
                              "Content-Type": "application/json"})
        response = conn.getresponse()
        data = json.loads(response.read())
        if response.status != 200 or [r["status"] for r in data["results"]] != (
                ["ACCEPTED"] * len(ops)):
            raise ValueError("SHADOW_AUTHENTICATED_HTTP_MUTATION_FAILED")
    finally:
        conn.close()


def operation(tag: str, kind: str, identifier: str, action: str, base: int,
              body: dict | None) -> dict:
    return {"op_id": tag, "kind": kind, "id": identifier,
            "action": action, "base_revision": base, "body": body}


def inspect(work: Path, port: int, deleted: bool) -> None:
    mirror = PcSharedMirror(work / "pc-shadow.sqlite", SITE, "pc-shadow-test",
                            f"http://127.0.0.1:{port}")
    data = mirror.snapshot()
    want_cursor = 5 if deleted else 3
    resources = {(r["kind"], r["id"]): r for r in data["resources"]}
    expected_keys = {("zone", "shadow-zone"), ("group", "shadow-group"),
                     ("zone_target", "shadow-zone")}
    if data["cursor"] != want_cursor or set(resources) != expected_keys:
        raise ValueError("SHADOW_PC_MIRROR_MISMATCH")
    zone = resources["zone", "shadow-zone"]
    group = resources["group", "shadow-group"]
    target = resources["zone_target", "shadow-zone"]
    if (group["body"] != GROUP or group["deleted"] or group["revision"] != 1):
        raise ValueError("SHADOW_PC_GROUP_MISMATCH")
    if deleted:
        if any(not resource["deleted"] or resource["body"] is not None or
               resource["revision"] != 2 for resource in (zone, target)):
            raise ValueError("SHADOW_TOMBSTONE_MISSING")
    elif (zone["deleted"] or target["deleted"] or zone["body"] != ZONE or
          target["body"] != TARGET or zone["revision"] != 1 or target["revision"] != 1):
        raise ValueError("SHADOW_PC_RESOURCE_MISMATCH")
    print("SHADOW_PC_SHARED_DATA_PASS: " +
          ("2 tombstones + 1 active group / cursor=5" if deleted else
           "zone + group + target / cursor=3"))


def main() -> int:
    parser = argparse.ArgumentParser(description="isolated S4D shadow data E2E")
    parser.add_argument("--work", required=True)
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--action", choices=("seed", "delete", "verify-before", "verify-after"),
                        required=True)
    args = parser.parse_args()
    work = Path(args.work)
    try:
        check_work(work)
        if not 1 <= args.port <= 65535:
            raise ValueError("SHADOW_PORT_INVALID")
        if args.action == "seed":
            token = enroll_writer(work)
            mutate(args.port, token, [
                operation("shadow-seed-zone", "zone", "shadow-zone", "UPSERT", 0, ZONE),
                operation("shadow-seed-group", "group", "shadow-group", "UPSERT", 0, GROUP),
                operation("shadow-seed-target", "zone_target", "shadow-zone", "UPSERT", 0, TARGET),
            ])
            print("SHADOW_OWNER_HTTP_3_WRITES_ACCEPTED")
        elif args.action == "delete":
            mutate(args.port, read_token(work), [
                operation("shadow-delete-target", "zone_target", "shadow-zone", "DELETE", 1, None),
                operation("shadow-delete-zone", "zone", "shadow-zone", "DELETE", 1, None),
            ])
            print("SHADOW_OWNER_HTTP_2_DELETES_ACCEPTED")
        elif args.action == "verify-before":
            inspect(work, args.port, False)
        elif args.action == "verify-after":
            inspect(work, args.port, True)
        return 0
    except Exception as exc:
        # No exceptions from the secret-bearing routines leave the command.
        print("SHADOW_DATA_FAILED: " + type(exc).__name__, file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
