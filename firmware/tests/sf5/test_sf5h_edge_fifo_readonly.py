#!/usr/bin/env python3
"""SF5H Edge SQLite readonly FIFO: exact-byte, corruption, restart and bounds."""
import importlib.util
from pathlib import Path
import sqlite3
import sys
import tempfile
from unittest.mock import patch

SOURCE = (Path(__file__).resolve().parents[3] /
          "edge" / "candidates" / "edge_durable_inbox.py")
spec = importlib.util.spec_from_file_location("sf5h_fifo_edge_inbox", SOURCE)
assert spec and spec.loader
mod = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = mod
spec.loader.exec_module(mod)


def frame(length: int, seed: int) -> bytes:
    data = bytearray((i * 11 + seed) & 0xFF for i in range(length))
    data[0:8] = bytes([2, 4, 2, 2 if length == 84 else 1,
                       2, 1, 48 if length == 84 else 64, 0])
    result = bytes(data)
    assert mod.admissible_product_frame(result)
    return result


def test_fifo_and_restart() -> None:
    with tempfile.TemporaryDirectory() as folder:
        path = Path(folder) / "edge.db"
        store = mod.EdgeDurableInbox(path, max_objects=4)
        assert store.oldest_batch(1).state == mod.ScanState.EMPTY
        a, b, c = frame(84, 12), frame(100, 34), frame(84, 56)
        ids = []
        for payload in (a, b, c):
            put = store.put(payload)
            assert put.state == mod.PutState.COMMITTED
            ids.append(put.sequence)
        assert ids == [1, 2, 3]

        result = store.oldest_batch(2)
        assert result.state == mod.ScanState.READY
        assert [(item.sequence, item.ciphertext) for item in result.items] == [
            (1, a), (2, b)]
        assert store.oldest_batch(64).items[-1] == mod.QueuedFrame(3, c)
        assert store.put(a).state == mod.PutState.DUPLICATE
        assert store.oldest_batch(4).items == (
            mod.QueuedFrame(1, a),
            mod.QueuedFrame(2, b),
            mod.QueuedFrame(3, c),
        )

        reopened = mod.EdgeDurableInbox(path, max_objects=4)
        assert reopened.oldest_batch(4).items == store.oldest_batch(4).items
        assert reopened.retained_count() == 3
        assert reopened.put(frame(100, 78)).state == mod.PutState.COMMITTED
        assert reopened.put(frame(84, 90)).state == mod.PutState.FULL
        assert len(reopened.oldest_batch(64).items) == 4


def test_future_corruption_refuses_whole_prefix() -> None:
    with tempfile.TemporaryDirectory() as folder:
        path = Path(folder) / "tampered.db"
        store = mod.EdgeDurableInbox(path)
        frames = [frame(84, 13), frame(100, 14), frame(84, 15)]
        for payload in frames:
            assert store.put(payload).state == mod.PutState.COMMITTED
        assert len(store.oldest_batch(1).items) == 1

        # Corrupt LAST item: even a request for only the first item must
        # yield NO prefix, so callers cannot accidentally stream a partial
        # supposedly clean database.
        with sqlite3.connect(path) as connection:
            changed = bytearray(frames[2])
            changed[35] ^= 1
            connection.execute(
                "UPDATE exact_frames SET ciphertext=? WHERE sequence=3",
                (sqlite3.Binary(bytes(changed)),))
        review = store.oldest_batch(1)
        assert review.state == mod.ScanState.REFUSED and review.items == ()
        assert store.retained_count() == 3


def test_shape_forgery_and_sequence_guard() -> None:
    with tempfile.TemporaryDirectory() as folder:
        path = Path(folder) / "shape.db"
        store = mod.EdgeDurableInbox(path)
        payload = frame(84, 17)
        assert store.put(payload).state == mod.PutState.COMMITTED

        # Recompute SHA index to show shape still matters independently;
        # this does not protect against a malicious fully rewritten DB.
        with sqlite3.connect(path) as connection:
            bogus = bytearray(payload)
            bogus[4] = 0
            wrong = bytes(bogus)
            connection.execute(
                "UPDATE exact_frames SET ciphertext=?,digest=? "
                "WHERE sequence=1",
                (sqlite3.Binary(wrong), sqlite3.Binary(mod.sha256(wrong).digest())))
        assert store.oldest_batch(1).state == mod.ScanState.REFUSED

    with tempfile.TemporaryDirectory() as folder:
        path = Path(folder) / "badseq.db"
        store = mod.EdgeDurableInbox(path)
        assert store.put(frame(100, 21)).state == mod.PutState.COMMITTED
        with sqlite3.connect(path) as connection:
            connection.execute("UPDATE exact_frames SET sequence=0 WHERE sequence=1")
        assert store.oldest_batch().state == mod.ScanState.REFUSED


def test_invalid_limits_and_read_failure() -> None:
    with tempfile.TemporaryDirectory() as folder:
        store = mod.EdgeDurableInbox(Path(folder) / "bounds.db")
        for invalid in (0, -1, 65, True, 1.5, "5", None):
            result = store.oldest_batch(invalid)
            assert result.state == mod.ScanState.INVALID and not result.items
        with patch.object(store, "_connect", side_effect=sqlite3.OperationalError("locked")):
            result = store.oldest_batch(5)
            assert result.state == mod.ScanState.UNAVAILABLE and not result.items

        with sqlite3.connect(store.path) as connection:
            connection.execute("DROP TABLE exact_frames")
        assert store.oldest_batch(5).state == mod.ScanState.UNAVAILABLE


def main() -> None:
    test_fifo_and_restart()
    test_future_corruption_refuses_whole_prefix()
    test_shape_forgery_and_sequence_guard()
    test_invalid_limits_and_read_failure()
    print("SF5H Edge FIFO reader: bounded-order/whole-scan/tamper/restart/fail-closed: PASS")


if __name__ == "__main__":
    main()
