#!/usr/bin/env python3
"""SF5H integration: Edge SQLite -> actual loopback HTTP -> backend SQLite."""
import importlib.util
from contextlib import contextmanager
from http.client import HTTPConnection
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
import sqlite3
import sys
import tempfile
from threading import Thread
from unittest.mock import patch

EDGE = Path(__file__).resolve().parents[3] / "edge" / "candidates"
sys.path.insert(0, str(EDGE))
import edge_durable_inbox as storemod  # noqa: E402
import loopback_backend_ingress as wire  # noqa: E402


def frame(n: int, seed: int) -> bytes:
    obj = bytearray((i * 17 + seed) & 255 for i in range(n))
    obj[0:8] = bytes([2, 4, 2, 2 if n == 84 else 1, 2,
                      1, 48 if n == 84 else 64, 0])
    result = bytes(obj)
    assert storemod.admissible_product_frame(result)
    return result


@contextmanager
def live_receiver(db_path: Path, capacity: int = 4096):
    server = wire.LoopbackBackendServer(db_path, max_objects=capacity)
    thread = Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield server
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)
        assert not thread.is_alive()


def test_real_http_and_restart_and_multiple_windows() -> None:
    with tempfile.TemporaryDirectory() as folder:
        root = Path(folder)
        source = storemod.EdgeDurableInbox(root / "edge.db", max_objects=90)
        original = []
        for index in range(70):
            obj = frame(84 if index % 2 == 0 else 100, 10 + index)
            original.append(obj)
            assert source.put(obj).state == storemod.PutState.COMMITTED
        with live_receiver(root / "backend-spool.db", capacity=90) as server:
            sender = wire.LoopbackEdgeSender(source, "127.0.0.1", server.port)
            # Explicit scan windows allow >64 without durable watermark.
            result = sender.send_window(limit=64)
            assert result.state == wire.TransferState.STAGED_UNTRUSTED
            assert result.staged_untrusted == 64 and result.next_volatile_cursor == 64
            result = sender.send_window(result.next_volatile_cursor, 64)
            assert result.state == wire.TransferState.STAGED_UNTRUSTED
            assert result.staged_untrusted == 6 and result.next_volatile_cursor == 70
            assert sender.send_window(70, 64).state == wire.TransferState.EMPTY
            assert server.store.retained_count() == 70
            assert source.retained_count() == 70  # no deletion on sending

            # Reboot sender or lose volatile cursor: replay whole queue.
            restarted_source = storemod.EdgeDurableInbox(root / "edge.db", 90)
            again = wire.LoopbackEdgeSender(restarted_source, "127.0.0.1", server.port)
            result = again.send_window(limit=64)
            assert result.state == wire.TransferState.STAGED_UNTRUSTED
            result = again.send_window(result.next_volatile_cursor, 64)
            assert result.staged_untrusted == 6
            assert server.store.retained_count() == 70
            with sqlite3.connect(server.store.path) as db:
                rows = db.execute(
                    "SELECT ciphertext FROM exact_frames ORDER BY sequence"
                ).fetchall()
            assert [bytes(row[0]) for row in rows] == original


def test_faults_retries_preserve_local_objects() -> None:
    with tempfile.TemporaryDirectory() as folder:
        root = Path(folder)
        source = storemod.EdgeDurableInbox(root / "source.db")
        a = frame(100, 70)
        assert source.put(a).state == storemod.PutState.COMMITTED
        with live_receiver(root / "server.db") as server:
            sender = wire.LoopbackEdgeSender(source, "127.0.0.1", server.port)
            server.next_fault = "before_commit"
            failed = sender.send_window()
            assert failed.state == wire.TransferState.REMOTE_FAILED
            assert failed.next_volatile_cursor == 0
            assert server.store.retained_count() == 0
            assert source.retained_count() == 1

            server.next_fault = "after_commit"  # persisted but reply uncertain
            failed = sender.send_window()
            assert failed.state == wire.TransferState.REMOTE_FAILED
            assert failed.next_volatile_cursor == 0
            assert server.store.retained_count() == 1
            assert source.retained_count() == 1

            recovered = sender.send_window()
            assert recovered.state == wire.TransferState.STAGED_UNTRUSTED
            assert recovered.next_volatile_cursor == 1
            assert server.store.retained_count() == 1


def test_capacity_and_unavailable_backend() -> None:
    with tempfile.TemporaryDirectory() as folder:
        root = Path(folder)
        source = storemod.EdgeDurableInbox(root / "source.db")
        assert source.put(frame(84, 3)).state == storemod.PutState.COMMITTED
        assert source.put(frame(84, 4)).state == storemod.PutState.COMMITTED
        with live_receiver(root / "server.db", capacity=1) as server:
            sender = wire.LoopbackEdgeSender(source, "127.0.0.1", server.port)
            report = sender.send_window(limit=2)
            assert report.state == wire.TransferState.REMOTE_FAILED
            assert report.attempted == 2 and report.staged_untrusted == 1
            assert report.next_volatile_cursor == 1
            assert server.store.retained_count() == 1
            assert source.retained_count() == 2
        # No server on the former port: source remains retained.
        report = sender.send_window()
        assert report.state == wire.TransferState.REMOTE_FAILED
        assert source.retained_count() == 2


def test_corrupt_source_prevents_any_network_send() -> None:
    with tempfile.TemporaryDirectory() as folder:
        root = Path(folder)
        source = storemod.EdgeDurableInbox(root / "source.db")
        assert source.put(frame(84, 10)).state == storemod.PutState.COMMITTED
        assert source.put(frame(100, 11)).state == storemod.PutState.COMMITTED
        with sqlite3.connect(source.path) as db:
            db.execute("UPDATE exact_frames SET ciphertext=? WHERE sequence=2",
                       (sqlite3.Binary(bytes([0]) * 100),))
        with live_receiver(root / "server.db") as server:
            sender = wire.LoopbackEdgeSender(source, "127.0.0.1", server.port)
            report = sender.send_window(limit=1)
            assert report.state == wire.TransferState.SOURCE_REFUSED
            assert report.attempted == 0 and server.store.retained_count() == 0


def test_invalid_inputs_and_forged_reply() -> None:
    with tempfile.TemporaryDirectory() as folder:
        root = Path(folder)
        source = storemod.EdgeDurableInbox(root / "source.db")
        assert source.put(frame(84, 25)).state == storemod.PutState.COMMITTED
        with live_receiver(root / "server.db") as server:
            for host, port in (("0.0.0.0", server.port),
                               ("localhost", server.port),
                               ("192.168.1.12", server.port),
                               ("127.0.0.1", 0),
                               ("127.0.0.1", True)):
                try:
                    wire.LoopbackEdgeSender(source, host, port)
                except ValueError:
                    pass
                else:
                    raise AssertionError("Unsafe sender accepted external host/port")
            sender = wire.LoopbackEdgeSender(source, "127.0.0.1", server.port)
            assert sender.send_window(limit=0).state == wire.TransferState.BAD_INPUT
            assert sender.send_window(after_sequence=-1).state == wire.TransferState.BAD_INPUT
            with patch.object(sender, "_send_one",
                              return_value=wire.TransferState.REMOTE_UNTRUSTED_RESPONSE):
                report = sender.send_window()
                assert report.state == wire.TransferState.REMOTE_UNTRUSTED_RESPONSE
                assert report.next_volatile_cursor == 0
            assert server.store.retained_count() == 0

            conn = HTTPConnection("127.0.0.1", server.port, timeout=1)
            conn.request("POST", "/v0/stage", body=b"bad",
                         headers={"Content-Type": "application/octet-stream"})
            answer = conn.getresponse()
            assert answer.status == 400
            answer.read()
            conn.close()
            assert server.store.retained_count() == 0


def test_invalid_response_is_not_staging_claim() -> None:
    # A local server is untrusted. Even its syntactically valid HTTP 200
    # is not a durable backend receipt and cannot release any object.
    class FakeHandler(BaseHTTPRequestHandler):
        def log_message(self, *args: object) -> None:
            pass
        def do_POST(self) -> None:
            size = int(self.headers["Content-Length"])
            self.rfile.read(size)
            body = b'{"kind":"sf5h-untrusted-staging-v0","status":"duplicate","sha256":"WRONG","size":84}'
            self.send_response(200)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    with tempfile.TemporaryDirectory() as folder:
        source = storemod.EdgeDurableInbox(Path(folder) / "edge.db")
        assert source.put(frame(84, 15)).state == storemod.PutState.COMMITTED
        server = HTTPServer(("127.0.0.1", 0), FakeHandler)
        thread = Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            sender = wire.LoopbackEdgeSender(source, "127.0.0.1", server.server_port)
            r = sender.send_window()
            assert r.state == wire.TransferState.REMOTE_UNTRUSTED_RESPONSE
            assert r.next_volatile_cursor == 0
            assert source.retained_count() == 1
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=2)
            assert not thread.is_alive()


def main() -> None:
    test_real_http_and_restart_and_multiple_windows()
    test_faults_retries_preserve_local_objects()
    test_capacity_and_unavailable_backend()
    test_corrupt_source_prevents_any_network_send()
    test_invalid_inputs_and_forged_reply()
    test_invalid_response_is_not_staging_claim()
    print("SF5H loopback backend ingress: live-HTTP/70-FIFO/retry/full/fault/no-release: PASS")


if __name__ == "__main__":
    main()
