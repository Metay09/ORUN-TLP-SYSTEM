"""S6J safety regression. All database files are synthetic test fixtures."""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import time
import unittest

from backend.shared_sync_store import Actor, SharedStore
from tools.ci.shadow_account_sqlite_migration_s6j import (
    PINNED_S4E, SITE, INSTALLATION, PORT, ShadowRefused, digest, rehearse,
)
from tools.ci.stage_account_runtime_source_s6i import ROOT, stage


class S6JShadowMigrationTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix="orun-s6j-fixture-")
        self.addCleanup(temp.cleanup)
        base = Path(temp.name)
        self.live = base / "live"
        self.candidate = base / "candidate"
        self.source = self.live / "state" / "backend"
        self.source.mkdir(parents=True, mode=0o700)
        self.live.chmod(0o700)
        (self.live / "state").chmod(0o700)
        self.source.chmod(0o700)
        self.live_manifest = self.live / "STAGING_MANIFEST.json"
        self.live_manifest.write_text(json.dumps({
            "site": SITE, "installation": INSTALLATION,
            "loopback_port": PORT, "source_commit": PINNED_S4E,
            "runtime_state": "STAGED_NOT_INSTALLED",
        }), encoding="utf-8")
        self.live_manifest.chmod(0o600)
        stage(self.candidate)
        self._old_registry()
        self._shared_data()

    def _old_registry(self):
        self.token_hash = hashlib.sha256(b"synthetic PC fixture").digest()
        path = self.source / "enrolled_edges.sqlite"
        with sqlite3.connect(path) as db:
            db.executescript("""
                CREATE TABLE edge_pending(
                    request_id TEXT PRIMARY KEY,
                    installation_id TEXT NOT NULL UNIQUE,
                    public_key BLOB NOT NULL, challenge BLOB NOT NULL,
                    expires_at INTEGER NOT NULL, owner_code_hash BLOB,
                    site_id TEXT, actor_id TEXT, scopes_json TEXT);
                CREATE TABLE edge_installs(
                    installation_id TEXT PRIMARY KEY,
                    public_key BLOB NOT NULL, site_id TEXT NOT NULL,
                    actor_id TEXT NOT NULL, scopes_json TEXT NOT NULL,
                    token_hash BLOB NOT NULL UNIQUE,
                    expires_at INTEGER NOT NULL, revoked_at INTEGER);
                CREATE INDEX edge_token_live
                ON edge_installs(token_hash,revoked_at);
            """)
            db.execute(
                "INSERT INTO edge_installs "
                "(installation_id,public_key,site_id,actor_id,scopes_json,"
                "token_hash,expires_at,revoked_at) "
                "VALUES(?,?,?,?,?,?,?,NULL)",
                (INSTALLATION, b"x" * 32, SITE, "owner",
                 '["zone:read"]', self.token_hash, int(time.time()) + 3600),
            )
        path.chmod(0o600)

    def _shared_data(self):
        path = self.source / "shared_resources.sqlite"
        store = SharedStore(path)
        actor = Actor(SITE, "owner", "fixture", frozenset({"zone:write"}))
        result = store.mutate(actor, [{
            "op_id": "synthetic-zone", "kind": "zone", "id": "z1",
            "action": "UPSERT", "base_revision": 0,
            "body": {
                "name": "Mera", "active": True,
                "ring_e7": [[376100000,280500000], [376100000,280510000],
                            [376110000,280510000], [376110000,280500000]],
            },
        }])
        self.assertEqual(result[0]["status"], "ACCEPTED")
        path.chmod(0o600)

    def test_shadow_migrates_only_copy_and_keeps_legacy_pc_hash(self):
        original = {p.name: digest(p) for p in self.source.glob("*.sqlite")}
        result = rehearse(self.live, self.candidate, source_root=ROOT)
        self.assertEqual(result["state"], "SHADOW_ONLY_NOT_DEPLOYABLE")
        self.assertEqual(result["owner_account_store"], "NOT_CREATED")
        self.assertEqual(result["live_pair_atomicity"], "NOT_PROVEN")
        self.assertEqual(original, {
            p.name: digest(p) for p in self.source.glob("*.sqlite")})
        base = self.candidate / "SHADOW_S6J"
        for name in ("baseline", "migration"):
            folder = base / name
            self.assertEqual(folder.stat().st_mode & 0o777, 0o700)
            with sqlite3.connect(folder / "enrolled_edges.sqlite") as db:
                row = db.execute(
                    "SELECT token_hash FROM edge_installs WHERE "
                    "installation_id=?", (INSTALLATION,)).fetchone()
                self.assertEqual(row[0], self.token_hash)
        with sqlite3.connect(
            base / "migration" / "enrolled_edges.sqlite") as db:
            self.assertEqual(db.execute(
                "SELECT key_algorithm FROM edge_installs").fetchone()[0],
                "Ed25519")
        self.assertFalse(
            (base / "migration" / "owner_accounts.sqlite").exists())
        with self.assertRaisesRegex(ShadowRefused, "SHADOW_ALREADY_EXISTS"):
            rehearse(self.live, self.candidate, source_root=ROOT)

    def test_missing_expected_pc_prevents_shadow_completion(self):
        with sqlite3.connect(self.source / "enrolled_edges.sqlite") as db:
            db.execute("UPDATE edge_installs SET revoked_at=?",
                       (int(time.time()),))
        with self.assertRaisesRegex(ShadowRefused, "EXPECTED_PC"):
            rehearse(self.live, self.candidate, source_root=ROOT)
        self.assertFalse((self.candidate / "SHADOW_S6J").exists())

    def test_live_manifest_and_symlink_fail_closed(self):
        original = json.loads(self.live_manifest.read_text())
        original["site"] = "wrong-site"
        self.live_manifest.write_text(json.dumps(original))
        with self.assertRaisesRegex(ShadowRefused, "LIVE_INSTALLATION_MISMATCH"):
            rehearse(self.live, self.candidate, source_root=ROOT)
        original["site"] = SITE
        self.live_manifest.write_text(json.dumps(original))
        file = self.source / "enrolled_edges.sqlite"
        file.rename(self.source / "enrolled.real")
        file.symlink_to(self.source / "enrolled.real")
        with self.assertRaisesRegex(ShadowRefused, "PRIVATE_PATH_UNSAFE"):
            rehearse(self.live, self.candidate, source_root=ROOT)
        self.assertFalse((self.candidate / "SHADOW_S6J").exists())

    def test_real_cli_imports_helpers_with_isolated_python_and_foreign_cwd(self):
        # Mirrors the user's direct Fish command rather than unittest imports.
        # -I removes the project root from Python's implicit sys.path.
        originals = {path.name: digest(path)
                     for path in self.source.glob("*.sqlite")}
        script = ROOT / "tools" / "ci" / "shadow_account_sqlite_migration_s6j.py"
        result = subprocess.run(
            [sys.executable, "-I", "-B", str(script),
             "--live-root", str(self.live),
             "--candidate", str(self.candidate)],
            cwd=self.live.parent,
            capture_output=True, text=True,
            timeout=20, check=False,
        )
        self.assertEqual(result.returncode, 0,
                         "isolated CLI invocation must not require PYTHONPATH")
        self.assertIn("ORUN_S6J_SHADOW_SCHEMA_REHEARSAL_PASS", result.stdout)
        self.assertNotIn("ModuleNotFoundError", result.stderr)
        self.assertEqual(
            originals,
            {path.name: digest(path) for path in self.source.glob("*.sqlite")})
        self.assertTrue(
            (self.candidate / "SHADOW_S6J" / "SHADOW_MANIFEST.json").is_file())

    def test_tampered_candidate_is_never_used_for_migration(self):
        candidate_file = (
            self.candidate / "runtime" / "backend" / "edge_enrollment.py")
        candidate_file.write_text("print('tampered')\n", encoding="utf-8")
        with self.assertRaisesRegex(ShadowRefused, "SOURCE_HASH_MISMATCH"):
            rehearse(self.live, self.candidate, source_root=ROOT)
        self.assertFalse((self.candidate / "SHADOW_S6J").exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
