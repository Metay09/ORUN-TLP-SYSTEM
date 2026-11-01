"""ORUN S6K — synthetic fixtures for S4E backward-read compatibility.

This test extracts pinned S4E source from the local Git object database.
It NEVER reads the actual ORUN-SHARED-RUNTIME or any owner credential.
"""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import sqlite3
import subprocess
import tempfile
import time
import unittest

from backend.shared_sync_store import Actor, SharedStore
from tools.ci.probe_s4e_shadow_rollback_s6k import (
    LEGACY_SOURCE, probe,
)
from tools.ci.shadow_account_sqlite_migration_s6j import (
    INSTALLATION, PINNED_S4E, PORT, SITE, ShadowRefused, digest, rehearse,
)
from tools.ci.stage_account_runtime_source_s6i import ROOT, stage

RING = [
    [376100000, 280500000], [376100000, 280510000],
    [376110000, 280510000], [376110000, 280500000],
]


class S6KLegacyRollbackReadTests(unittest.TestCase):
    def setUp(self) -> None:
        temp = tempfile.TemporaryDirectory(prefix="orun-s6k-fixture-")
        self.addCleanup(temp.cleanup)
        home = Path(temp.name)
        self.live = home / "pinned"
        self.candidate = home / "candidate"
        for directory in (
            self.live,
            self.live / "runtime",
            self.live / "runtime" / "backend",
            self.live / "runtime" / "tools",
            self.live / "state",
            self.live / "state" / "backend",
        ):
            directory.mkdir(mode=0o700, exist_ok=True)
        hashes = {}
        for name in LEGACY_SOURCE:
            data = subprocess.check_output(
                ["git", "-C", str(ROOT), "show", PINNED_S4E + ":" + name],
                timeout=10,
            )
            target = self.live / "runtime" / name
            target.write_bytes(data)
            target.chmod(0o600)
            hashes[name] = hashlib.sha256(data).hexdigest()
        manifest = {
            "site": SITE, "installation": INSTALLATION, "loopback_port": PORT,
            "source_commit": PINNED_S4E,
            "runtime_state": "STAGED_NOT_INSTALLED",
            "source_sha256": hashes,
        }
        source = self.live / "STAGING_MANIFEST.json"
        source.write_text(json.dumps(manifest), encoding="utf-8")
        source.chmod(0o600)
        self._old_registry()
        self._shared_store()
        stage(self.candidate)
        rehearse(self.live, self.candidate, source_root=ROOT)

    def _old_registry(self) -> None:
        dbfile = self.live / "state" / "backend" / "enrolled_edges.sqlite"
        with sqlite3.connect(dbfile) as db:
            db.executescript("""
              CREATE TABLE edge_pending (
                request_id TEXT PRIMARY KEY,
                installation_id TEXT NOT NULL UNIQUE,
                public_key BLOB NOT NULL,
                challenge BLOB NOT NULL,
                expires_at INTEGER NOT NULL,
                owner_code_hash BLOB, site_id TEXT,
                actor_id TEXT, scopes_json TEXT);
              CREATE TABLE edge_installs (
                installation_id TEXT PRIMARY KEY,
                public_key BLOB NOT NULL, site_id TEXT NOT NULL,
                actor_id TEXT NOT NULL, scopes_json TEXT NOT NULL,
                token_hash BLOB NOT NULL UNIQUE,
                expires_at INTEGER NOT NULL, revoked_at INTEGER);
              CREATE INDEX edge_token_live
                ON edge_installs(token_hash, revoked_at);
            """)
            db.execute(
                "INSERT INTO edge_installs "
                "(installation_id,public_key,site_id,actor_id,scopes_json,"
                "token_hash,expires_at,revoked_at) VALUES(?,?,?,?,?,?,?,NULL)",
                (INSTALLATION, b"p" * 32, SITE, "owner",
                 json.dumps(["zone:read"]), hashlib.sha256(
                     b"synthetic fixture only").digest(),
                 int(time.time()) + 3600),
            )
        dbfile.chmod(0o600)

    def _shared_store(self) -> None:
        dbfile = self.live / "state" / "backend" / "shared_resources.sqlite"
        store = SharedStore(dbfile)
        actor = Actor(SITE, "owner", "synthetic", frozenset({"zone:write"}))
        result = store.mutate(actor, [{
            "op_id": "s6k-fixture", "kind": "zone", "id": "z1",
            "action": "UPSERT", "base_revision": 0,
            "body": {"name": "Mera", "active": True, "ring_e7": RING},
        }])
        self.assertEqual(result[0]["status"], "ACCEPTED")
        dbfile.chmod(0o600)

    def test_pinned_s4e_reads_migrated_shadow_and_preserves_all_sources(self):
        paths = list((self.live / "state" / "backend").glob("*.sqlite"))
        paths += list((self.candidate / "SHADOW_S6J").rglob("*.sqlite"))
        before = {str(p): digest(p) for p in paths}
        probe(self.live, self.candidate, root=ROOT)
        self.assertEqual(before, {str(p): digest(p) for p in paths})
        self.assertEqual(
            list(self.candidate.glob("S6K_ROLLBACK_PROBE_*")), [])

    def test_tampered_pinned_runtime_fails_before_any_probe(self):
        file = self.live / "runtime" / "backend" / "edge_enrollment.py"
        file.write_text("malicious code", encoding="utf-8")
        with self.assertRaisesRegex(ShadowRefused, "PINNED_S4E_SOURCE_HASH"):
            probe(self.live, self.candidate, root=ROOT)
        self.assertEqual(
            list(self.candidate.glob("S6K_ROLLBACK_PROBE_*")), [])

    def test_modified_shadow_database_fails_its_sha256_gate(self):
        path = (self.candidate / "SHADOW_S6J" / "migration" /
                "enrolled_edges.sqlite")
        with path.open("ab") as output:
            output.write(b"tamper")
        with self.assertRaisesRegex(ShadowRefused, "SHADOW_DATABASE_HASH"):
            probe(self.live, self.candidate, root=ROOT)
        self.assertEqual(
            list(self.candidate.glob("S6K_ROLLBACK_PROBE_*")), [])


if __name__ == "__main__":
    unittest.main(verbosity=2)
