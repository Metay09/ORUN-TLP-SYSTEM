"""Focused, disposable cursor-feed contract tests (no live services)."""
from __future__ import annotations

import http.client
import json
import sqlite3
from pathlib import Path
from types import SimpleNamespace
import tempfile
from threading import Thread
import unittest

from backend.observation_read_page import (
    MAX_CURSOR, PAGE_SIZE, ObservationReadUnavailable, read_page,
)
from backend.orun_backend_bench import make_server, load_private_token_file
from backend.account_observation_relay import AccountObservationRelay, RelayUnavailable


def fixture(db: Path, count: int) -> None:
    with sqlite3.connect(db) as c:
        c.executescript("""
            CREATE TABLE verified_observations (
              device_id BLOB, incarnation BLOB, sequence INTEGER, family INTEGER,
              exact_first_frame BLOB);
            CREATE TABLE edge_receipts (
              device_id BLOB, incarnation BLOB, sequence INTEGER,
              first_seen_utc INTEGER, gw_rssi INTEGER, gw_snr INTEGER);
        """)
        for i in range(1, count + 1):
            c.execute("INSERT INTO verified_observations VALUES (?,?,?,?,?)",
                      (bytes.fromhex("52E16773141A8887"),
                       bytes.fromhex("5523010CEA37319F"), i, 1, bytes([i % 256]) * 100))
            c.execute("INSERT INTO edge_receipts VALUES (?,?,?,?,?,?)",
                      (bytes.fromhex("52E16773141A8887"),
                       bytes.fromhex("5523010CEA37319F"), i, 1800000000 + i, -90, 8))


class CursorReadTests(unittest.TestCase):
    def test_exact_pages_and_readonly(self) -> None:
        with tempfile.TemporaryDirectory() as t:
            db = Path(t) / "backend.sqlite"
            fixture(db, PAGE_SIZE + 4)
            one = read_page(db, 0)
            self.assertEqual(len(one["records"]), PAGE_SIZE)
            self.assertEqual(one["cursor"], PAGE_SIZE)
            self.assertTrue(one["has_more"])
            self.assertEqual(one["records"][0]["device"], "52E16773141A8887")
            self.assertEqual(len(one["records"][0]["frame"]), 200)
            self.assertEqual(one["records"][0]["rssi"], -90)
            two = read_page(db, one["cursor"])
            self.assertEqual(len(two["records"]), 4)
            self.assertFalse(two["has_more"])
            self.assertEqual(two["cursor"], PAGE_SIZE + 4)
            self.assertEqual(read_page(db, two["cursor"])["records"], [])
            with sqlite3.connect(db) as c:
                self.assertEqual(c.execute("SELECT count(*) FROM verified_observations").fetchone()[0], PAGE_SIZE + 4)

    def test_cursor_rejection_and_corrupt_storage_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as t:
            db = Path(t) / "backend.sqlite"
            with self.assertRaises(ObservationReadUnavailable):
                read_page(db, 0)
            fixture(db, 1)
            for value in (-1, 1.0, True, MAX_CURSOR + 1):
                with self.assertRaises(ValueError):
                    read_page(db, value)
            with sqlite3.connect(db) as c:
                c.execute("UPDATE verified_observations SET exact_first_frame=X'00'")
            with self.assertRaises(ObservationReadUnavailable):
                read_page(db, 0)

    def test_http_needs_bearer_and_valid_cursor(self) -> None:
        with tempfile.TemporaryDirectory() as t:
            db = Path(t) / "backend.sqlite"
            fixture(db, 2)
            backend = SimpleNamespace(db_path=db)
            server = make_server(backend, "127.0.0.1", 0, "only-test-token")
            thread = Thread(target=server.serve_forever, daemon=True)
            thread.start()
            try:
                def get(route: str, authorization: str | None):
                    c = http.client.HTTPConnection("127.0.0.1", server.server_port, timeout=3)
                    try:
                        headers = {"Authorization": authorization} if authorization else {}
                        c.request("GET", route, headers=headers)
                        r = c.getresponse()
                        return r.status, json.loads(r.read())
                    finally:
                        c.close()
                route = "/v1/edge/records?after=0"
                self.assertEqual(get(route, None)[0], 401)
                self.assertEqual(get(route, "Bearer wrong")[0], 401)
                self.assertEqual(get("/v1/edge/records?after=0&more=1", "Bearer only-test-token")[0], 400)
                self.assertEqual(get("/v1/edge/records?after=-1", "Bearer only-test-token")[0], 400)
                code, page = get(route, "Bearer only-test-token")
                self.assertEqual(code, 200)
                self.assertEqual([r["sequence"] for r in page["records"]], [1, 2])
            finally:
                server.shutdown()
                server.server_close()
                thread.join(timeout=3)

    def test_relay_must_be_explicitly_site_bound(self) -> None:
        relay = AccountObservationRelay.__new__(AccountObservationRelay)
        relay.observation_site = None
        with self.assertRaises(RelayUnavailable):
            relay.forward("GET", "/v1/edge/records?after=0")
        relay.observation_site = "orun-main"
        for bad in (
            "/v1/edge/records?after=-1",
            "/v1/edge/records?after=0&other=1",
            "/v1/edge/records?after=999999999999999999999",
            "/api/points?after=0",
        ):
            with self.assertRaises(RelayUnavailable):
                relay.forward("GET", bad)


    def test_private_token_file_accepts_owner_only_file(self) -> None:
        with tempfile.TemporaryDirectory() as t:
            path = Path(t) / "service-token"
            path.write_text("SYNTHETIC-ONLY-TEST-BEARER-12345\\n", encoding="utf-8")
            path.chmod(0o600)
            self.assertEqual(
                load_private_token_file(path), "SYNTHETIC-ONLY-TEST-BEARER-12345")

    def test_private_token_file_rejects_public_and_symlink(self) -> None:
        with tempfile.TemporaryDirectory() as t:
            path = Path(t) / "service-token"
            path.write_text("SYNTHETIC-ONLY-TEST-BEARER-12345", encoding="utf-8")
            path.chmod(0o644)
            with self.assertRaises(ValueError):
                load_private_token_file(path)
            path.chmod(0o600)
            link = Path(t) / "service-token-link"
            link.symlink_to(path)
            with self.assertRaises(OSError):
                load_private_token_file(link)

    def test_private_token_file_rejects_oversize_and_invalid_content(self) -> None:
        with tempfile.TemporaryDirectory() as t:
            path = Path(t) / "service-token"
            path.write_bytes(b"X" * 4097)
            path.chmod(0o600)
            with self.assertRaises(ValueError):
                load_private_token_file(path)
            path.write_text("short", encoding="utf-8")
            with self.assertRaises(ValueError):
                load_private_token_file(path)
            path.write_text("HAS SPACE IN TOKEN 12345", encoding="utf-8")
            with self.assertRaises(ValueError):
                load_private_token_file(path)


if __name__ == "__main__":
    unittest.main()
