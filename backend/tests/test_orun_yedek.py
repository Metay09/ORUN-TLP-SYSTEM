"""5. adım: sunucu yedeği ve yedekten dönüş provası (tools/orun_yedek.py)."""
from __future__ import annotations

import json
import os
from pathlib import Path
import sqlite3
import stat
import tempfile
import unittest
from unittest import mock

from backend.account_login import AccountStore
from backend.tests.test_account_device_login import PASSWORD
from backend.tests.test_observation_latest import A, INC, periodic
from tools import orun_yedek as y
from tools.orun_shared_sync_loopback import prepare
from tools.orun_sunucu import build

TOKEN = "SYNTHETIC-BRIDGE-TOKEN-1234567890"


class YedekTests(unittest.TestCase):
    def setUp(self) -> None:
        tmp = tempfile.TemporaryDirectory(prefix="orun-yedek-test-")
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        state = self.root / "state"
        state.mkdir(mode=0o700)
        prepare(state, reuse_state=False)
        accounts = state / "owner_accounts.sqlite"
        AccountStore(accounts).bootstrap_owner("orun-main", "owner", "ownerlogin", PASSWORD)
        os.chmod(accounts, 0o600)
        self.db = self.root / "ledger.sqlite"
        self.registry = self.root / "registry.sqlite"
        local, _ = build(state, self.db, self.registry, "orun-main")
        self.local = local
        self.insert(1, 1_700_000_900)
        self.insert(2, 1_700_000_100)
        token = self.root / "bridge.token"
        token.write_text(TOKEN + "\n")
        os.chmod(token, 0o600)
        self.src = {"state_dir": str(state), "db": str(self.db),
                    "registry": str(self.registry), "site": "orun-main",
                    "token_file": str(token)}
        self.out = self.root / "yedekler"

    def insert(self, seq: int, t: int) -> None:
        with sqlite3.connect(self.db) as c:
            c.execute("INSERT INTO verified_observations "
                      "(device_id,incarnation,sequence,family,plaintext,exact_first_frame) "
                      "VALUES (?,?,?,?,?,?)", (A, INC, seq, 1, periodic(t), bytes([seq % 250]) * 100))

    def take(self, **kw) -> Path:
        return y.take(self.src, self.out, kw.pop("keep", 14), **kw)

    def test_backup_is_private_complete_and_restorable(self):
        backup = self.take(now=1_800_000_000)
        self.assertEqual(stat.S_IMODE(self.out.stat().st_mode), 0o700)
        self.assertEqual(stat.S_IMODE(backup.stat().st_mode), 0o700)
        for path in backup.rglob("*"):
            if path.is_file():
                self.assertEqual(stat.S_IMODE(path.stat().st_mode) & 0o077, 0, path)
        manifest = y.load_manifest(backup)
        files = manifest["files"]
        self.assertEqual(files["kayit_defteri.sqlite"]["tables"]["verified_observations"], 2)
        self.assertIn("hesap/owner_accounts.sqlite", files)
        self.assertIn("kimlik_defteri.sqlite", files)
        self.assertEqual(files["kopru_anahtari"]["kind"], "file")
        # No SQLite side files travel with the snapshot.
        self.assertFalse([p.name for p in backup.rglob("*") if p.name.endswith(y.SIDE_FILES)])

        lines: list[str] = []
        report = y.rehearse(backup, log=lines.append)
        self.assertEqual(report["records"], 2)
        self.assertEqual(report["latest"], 1)      # one collar, its newest position
        self.assertEqual(report["users"], 1)
        self.assertEqual(len(lines), 4)

    def test_rehearsal_never_touches_live_or_backup(self):
        backup = self.take()
        def fingerprint(folder: Path) -> dict:
            return {p.relative_to(folder).as_posix(): (p.stat().st_size, y.sha256(p))
                    for p in folder.rglob("*") if p.is_file()}
        live_before = {p: y.sha256(Path(self.src[p])) for p in ("db", "registry", "token_file")}
        backup_before = fingerprint(backup)
        y.rehearse(backup, log=lambda _line: None)
        self.assertEqual(fingerprint(backup), backup_before)
        self.assertEqual({p: y.sha256(Path(self.src[p])) for p in live_before}, live_before)
        self.assertFalse(list(Path(tempfile.gettempdir()).glob("orun-prova-*")))

    def test_snapshot_includes_rows_still_only_in_the_wal(self):
        # A plain file copy of the main database file would miss these rows.
        writer = sqlite3.connect(self.db)
        self.addCleanup(writer.close)
        writer.execute("PRAGMA journal_mode=WAL")
        writer.execute("PRAGMA wal_autocheckpoint=0")
        for seq in range(3, 43):
            writer.execute("INSERT INTO verified_observations "
                           "(device_id,incarnation,sequence,family,plaintext,exact_first_frame) "
                           "VALUES (?,?,?,?,?,?)", (A, INC, seq, 1, periodic(1_700_001_000 + seq),
                                                    bytes([seq]) * 100))
        writer.commit()
        self.assertGreater(Path(str(self.db) + "-wal").stat().st_size, 0)
        naive = self.root / "naive.sqlite"
        naive.write_bytes(self.db.read_bytes())           # what "cp" would take
        with sqlite3.connect(naive) as c:
            naive_rows = c.execute("SELECT COUNT(*) FROM verified_observations").fetchone()[0]
        self.assertLess(naive_rows, 42)

        backup = self.take()
        tables = y.load_manifest(backup)["files"]["kayit_defteri.sqlite"]["tables"]
        self.assertEqual(tables["verified_observations"], 42)
        self.assertEqual(y.rehearse(backup, log=lambda _l: None)["records"], 42)

    def test_backup_is_one_instant_later_writes_are_not_in_it(self):
        backup = self.take()
        self.insert(99, 1_700_009_000)
        self.assertEqual(y.rehearse(backup, log=lambda _l: None)["records"], 2)

    def test_works_when_server_is_stopped_and_wal_is_gone(self):
        with sqlite3.connect(self.db) as c:
            c.execute("PRAGMA wal_checkpoint(TRUNCATE)")
        for suffix in ("-wal", "-shm"):
            Path(str(self.db) + suffix).unlink(missing_ok=True)
        backup = self.take()
        self.assertEqual(y.rehearse(backup, log=lambda _l: None)["records"], 2)

    def test_damaged_or_altered_backup_fails_rehearsal(self):
        backup = self.take()
        ledger = backup / "kayit_defteri.sqlite"
        data = bytearray(ledger.read_bytes())
        data[len(data) // 2] ^= 0xFF
        ledger.write_bytes(bytes(data))
        with self.assertRaises(y.YedekHatasi):
            y.rehearse(backup, log=lambda _l: None)

        other = self.take(now=1_800_000_100)
        (other / "hesap" / "owner_accounts.sqlite").unlink()
        with self.assertRaises(y.YedekHatasi):
            y.rehearse(other, log=lambda _l: None)

        third = self.take(now=1_800_000_200)
        (third / "fazladan").write_text("x")
        with self.assertRaises(y.YedekHatasi):
            y.rehearse(third, log=lambda _l: None)

    def test_row_count_drift_is_detected(self):
        backup = self.take()
        with sqlite3.connect(backup / "kayit_defteri.sqlite") as c:
            c.execute("DELETE FROM verified_observations WHERE sequence=2")
        manifest = json.loads((backup / y.MANIFEST).read_text())
        entry = manifest["files"]["kayit_defteri.sqlite"]
        entry["bytes"] = (backup / "kayit_defteri.sqlite").stat().st_size
        entry["sha256"] = y.sha256(backup / "kayit_defteri.sqlite")   # hash "fixed", counts not
        (backup / y.MANIFEST).write_text(json.dumps(manifest))
        with self.assertRaisesRegex(y.YedekHatasi, "kayıt sayıları"):
            y.rehearse(backup, log=lambda _l: None)

    def test_broken_relations_are_reported(self):
        with sqlite3.connect(self.registry) as c:
            c.execute("INSERT INTO grants VALUES ('0E8ADE7E71531AA3', 7, '09A462BD4B275BA5', 0, 1, 1)")
        backup = self.take()
        with self.assertRaisesRegex(y.YedekHatasi, "ilişkiler"):
            y.rehearse(backup, log=lambda _l: None)

    def test_failed_backup_leaves_no_half_backup_and_is_visible(self):
        missing = dict(self.src, registry=str(self.root / "yok.sqlite"))
        with self.assertRaises(y.YedekHatasi):
            y.take(missing, self.out, 14)
        self.assertEqual(y.backups(self.out), [])
        self.assertEqual([p.name for p in self.out.iterdir()], [])   # no .yarim folder left

        code = y.main(["al", "--state-dir", missing["state_dir"], "--db", missing["db"],
                       "--registry", missing["registry"], "--site", "orun-main",
                       "--hedef", str(self.out)])
        self.assertEqual(code, 2)
        self.assertFalse(json.loads((self.out / y.STATUS).read_text())["ok"])
        self.assertEqual(y.main(["durum", "--hedef", str(self.out)]), 1)

    def test_status_ok_then_stale(self):
        code = y.main(["al", "--state-dir", self.src["state_dir"], "--db", self.src["db"],
                       "--registry", self.src["registry"], "--site", "orun-main",
                       "--token-file", self.src["token_file"], "--hedef", str(self.out)])
        self.assertEqual(code, 0)
        self.assertEqual(y.main(["durum", "--hedef", str(self.out)]), 0)
        self.assertEqual(y.main(["prova", "--hedef", str(self.out)]), 0)
        state = json.loads((self.out / y.STATUS).read_text())
        state["time_utc"] -= 30 * 3600
        (self.out / y.STATUS).write_text(json.dumps(state))
        self.assertEqual(y.main(["durum", "--hedef", str(self.out)]), 1)

    def test_retention_keeps_only_the_newest_complete_backups(self):
        names = [self.take(now=1_800_000_000 + i * 60, keep=3).name for i in range(5)]
        self.assertEqual([p.name for p in y.backups(self.out)], names[-3:])
        # A folder that is not a completed backup is never deleted or counted.
        stray = self.out / "notlarim"
        stray.mkdir()
        self.take(now=1_800_001_000, keep=1)
        self.assertTrue(stray.is_dir())
        self.assertEqual(len(y.backups(self.out)), 1)

    def test_sources_come_from_the_running_service(self):
        shown = ("{ path=/usr/bin/python3 ; argv[]=/usr/bin/python3 "
                 "/home/u/ORUN-RELEASES/sunucu-abc/tools/orun_sunucu.py "
                 "--state-dir /home/u/.local/share/orun/state --db /home/u/orun-backend-bench.sqlite "
                 "--registry /home/u/orun-credentials.sqlite --site orun-main "
                 "--listen 127.0.0.1:18781 --bridge-listen 100.64.0.1:8090 "
                 "--bridge-token-file /home/u/.config/orun/bridge_token ; ignore_errors=no ; "
                 "start_time=[n/a] ; stop_time=[n/a] ; pid=0 ; code=(null) ; status=0/0 }\n")

        class Done:
            returncode, stdout = 0, shown

        with mock.patch.object(y.subprocess, "run", return_value=Done()) as run:
            src = y.unit_sources("orun-sunucu.service")
        self.assertEqual(run.call_args[0][0][:3], ["systemctl", "--user", "show"])
        self.assertEqual(src, {
            "state_dir": "/home/u/.local/share/orun/state",
            "db": "/home/u/orun-backend-bench.sqlite",
            "registry": "/home/u/orun-credentials.sqlite", "site": "orun-main",
            "token_file": "/home/u/.config/orun/bridge_token"})

        class Missing:
            returncode, stdout = 0, ""

        with mock.patch.object(y.subprocess, "run", return_value=Missing()):
            with self.assertRaises(y.YedekHatasi):
                y.unit_sources("orun-sunucu.service")

    def test_paths_must_be_given_together(self):
        self.assertEqual(y.main(["al", "--db", self.src["db"], "--hedef", str(self.out)]), 2)


if __name__ == "__main__":
    unittest.main()
