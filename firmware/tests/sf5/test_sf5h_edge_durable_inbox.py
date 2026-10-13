#!/usr/bin/env python3
"""Host-only SF5H SQLite exact sealed-object persistence regression."""
import importlib.util
from pathlib import Path
import sqlite3
import tempfile
from unittest.mock import patch

MODPATH = (Path(__file__).resolve().parents[3] /
           "edge" / "candidates" / "edge_durable_inbox.py")
spec = importlib.util.spec_from_file_location("sf5h_edge_inbox_candidate", MODPATH)
assert spec and spec.loader
module = importlib.util.module_from_spec(spec)
import sys
sys.modules[spec.name] = module
spec.loader.exec_module(module)

EdgeDurableInbox = module.EdgeDurableInbox
PutState = module.PutState


def frame(length: int, seed: int, *, relay: bool = False) -> bytes:
    data = bytearray((i * 17 + seed) & 0xFF for i in range(length))
    data[0] = 2
    data[1] = 4
    data[2] = 2
    data[3] = 2 if length == 84 else 1
    data[4] = 3 if relay else 2
    data[5] = 1
    data[6] = 48 if length == 84 else 64
    data[7] = 0
    result = bytes(data)
    assert module.admissible_product_frame(result)
    return result


def test_exact_bytes_and_restart() -> None:
    with tempfile.TemporaryDirectory() as folder:
        path = Path(folder) / "edge.sqlite"
        inbox = EdgeDurableInbox(path, max_objects=2)
        a = frame(84, 12)
        b = frame(100, 24, relay=True)

        r = inbox.put(a)
        assert r.state == PutState.COMMITTED and r.sequence == 1
        assert inbox.put(a).state == PutState.DUPLICATE
        assert inbox.retained_count() == 1

        reopened = EdgeDurableInbox(path, max_objects=2)
        assert reopened.put(a).state == PutState.DUPLICATE
        r = reopened.put(b)
        assert r.state == PutState.COMMITTED and r.sequence == 2
        assert reopened.retained_count() == 2
        assert reopened.put(frame(100, 60)).state == PutState.FULL
        assert reopened.put(a).state == PutState.DUPLICATE

        with sqlite3.connect(path) as conn:
            stored = conn.execute(
                "SELECT ciphertext,size FROM exact_frames ORDER BY sequence"
            ).fetchall()
            mode = conn.execute("PRAGMA journal_mode").fetchone()[0]
            sync = conn.execute("PRAGMA synchronous").fetchone()[0]
            assert stored == [(a, 84), (b, 100)]
            assert mode.lower() == "wal" and sync == 2


def test_crash_before_and_after_commit() -> None:
    with tempfile.TemporaryDirectory() as folder:
        path = Path(folder) / "inbox.db"
        first = EdgeDurableInbox(path, max_objects=1)
        a = frame(100, 42)
        assert first.put(a, fault="before_commit").state == PutState.UNCERTAIN
        assert EdgeDurableInbox(path, max_objects=1).retained_count() == 0

        assert first.put(a, fault="after_commit").state == PutState.UNCERTAIN
        reboot = EdgeDurableInbox(path, max_objects=1)
        assert reboot.retained_count() == 1
        assert reboot.put(a).state == PutState.DUPLICATE
        assert reboot.put(frame(84, 99)).state == PutState.FULL
        assert reboot.put(a).state == PutState.DUPLICATE


def test_uncertain_readback_must_not_claim_success() -> None:
    with tempfile.TemporaryDirectory() as folder:
        path = Path(folder) / "inbox.sqlite"
        inbox = EdgeDurableInbox(path)
        original = frame(100, 51)
        with patch.object(inbox, "_readback", return_value=False):
            assert inbox.put(original).state == PutState.UNCERTAIN
        assert EdgeDurableInbox(path).put(original).state == PutState.DUPLICATE


def test_index_collision_requires_exact_bytes() -> None:
    class ConstantHash:
        def digest(self) -> bytes:
            return bytes([0x11]) * 32

    with tempfile.TemporaryDirectory() as folder:
        inbox = EdgeDurableInbox(Path(folder) / "hashcollision.db")
        original_sha = module.sha256
        del original_sha  # fixture intentionally models an unlikely digest collision
        with patch.object(module, "sha256", return_value=ConstantHash()):
            a, b = frame(84, 44), frame(84, 45)
            assert inbox.put(a).state == PutState.COMMITTED
            assert inbox.put(b).state == PutState.COMMITTED
            assert inbox.put(a).state == PutState.DUPLICATE
            assert inbox.retained_count() == 2


def test_corrupted_index_refused_no_duplicate_or_insert() -> None:
    with tempfile.TemporaryDirectory() as folder:
        path = Path(folder) / "tamper.db"
        inbox = EdgeDurableInbox(path)
        obj = frame(84, 16)
        assert inbox.put(obj).state == PutState.COMMITTED
        with sqlite3.connect(path) as conn:
            damaged = bytearray(obj)
            damaged[31] ^= 1
            conn.execute(
                "UPDATE exact_frames SET ciphertext=? WHERE sequence=1",
                (sqlite3.Binary(bytes(damaged)),),
            )
        assert inbox.put(obj).state == PutState.REJECTED
        assert inbox.retained_count() == 1


def test_shape_and_database_paths() -> None:
    a = frame(84, 9)
    b = frame(100, 11)
    with tempfile.TemporaryDirectory() as folder:
        inbox = EdgeDurableInbox(Path(folder) / "validate.db")
        variants = [b"", b"hello", a[:-1], b + b"0"]
        for index, value in [
            (0, 3), (1, 8), (2, 1), (3, 1), (4, 0), (4, 1),
            (4, 4), (5, 3), (6, 66), (7, 1)
        ]:
            changed = bytearray(a)
            changed[index] = value
            variants.append(bytes(changed))
        changed = bytearray(a)
        changed[16:20] = b"\\xff" * 4
        variants.append(bytes(changed))
        changed = bytearray(a)
        changed[20:28] = b"\\x00" * 8
        variants.append(bytes(changed))
        for value in variants:
            assert inbox.put(value).state == PutState.INVALID
        assert inbox.retained_count() == 0
        assert inbox.put(a, fault="not-a-fault").state == PutState.INVALID
        for path in ["", ":memory:", "file:memory?cache=shared"]:
            try:
                EdgeDurableInbox(path)
            except ValueError:
                pass
            else:
                raise AssertionError("Accepted nondurable SQLite URI")
        for capacity in [0, -1, 100001]:
            try:
                EdgeDurableInbox(Path(folder) / "other.sqlite", capacity)
            except ValueError:
                pass
            else:
                raise AssertionError("Accepted unbounded/invalid capacity")


def main() -> None:
    test_exact_bytes_and_restart()
    test_crash_before_and_after_commit()
    test_uncertain_readback_must_not_claim_success()
    test_index_collision_requires_exact_bytes()
    test_corrupted_index_refused_no_duplicate_or_insert()
    test_shape_and_database_paths()
    print("SF5H Edge SQLite inbox: exact-durable/readback/restart/full/fault/collision: PASS")


if __name__ == "__main__":
    main()
