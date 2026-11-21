"""1. adım: tasma başına en son ÖLÇÜLEN konum (varış sırası değil)."""
from __future__ import annotations

import http.client
import json
from pathlib import Path
import sqlite3
import tempfile
from threading import Thread
import unittest

from backend.account_observation_relay import AccountObservationRelay, RelayUnavailable
from backend.observation_read_page import LIVE_REPORT_CUTOVER_UTC, read_latest, read_page
from tools.orun_observation_readonly_sidecar import make_readonly_server

A = bytes.fromhex("52E16773141A8887")
B = bytes.fromhex("0011223344556677")
INC = bytes.fromhex("5523010CEA37319F")
TOKEN = "SYNTHETIC-LATEST-TEST-BEARER-123456"


def periodic(measured: int, located: bool = True) -> bytes:
    p = bytearray(64)
    # A missing GNSS clock has neither the time-valid nor location-valid bit.
    # The live ORUN-1 frames (7714..7718) carry flags=0 and utc=0.
    p[1] = 0 if measured == 0 else (0b11 if located else 0b01)
    p[16:20] = measured.to_bytes(4, "big")
    return bytes(p)


def ledger(db: Path, rows: list[tuple[bytes, int, int, bytes]]) -> None:
    """rows: (device, sequence, family, plaintext) in ARRIVAL order."""
    with sqlite3.connect(db) as c:
        c.executescript("""
            CREATE TABLE verified_observations (
              device_id BLOB, incarnation BLOB, sequence INTEGER, family INTEGER,
              plaintext BLOB, exact_first_frame BLOB);
            CREATE TABLE edge_receipts (
              device_id BLOB, incarnation BLOB, sequence INTEGER,
              first_seen_utc INTEGER, gw_rssi INTEGER, gw_snr INTEGER);
        """)
        for dev, seq, family, plain in rows:
            c.execute("INSERT INTO verified_observations VALUES (?,?,?,?,?,?)",
                      (dev, INC, seq, family, plain, bytes([seq]) * 100))
            c.execute("INSERT INTO edge_receipts VALUES (?,?,?,?,?,?)",
                      (dev, INC, seq, 1800000000 + seq, -90, 8))


class LatestTests(unittest.TestCase):
    def setUp(self) -> None:
        temp = tempfile.TemporaryDirectory(prefix="orun-latest-test-")
        self.addCleanup(temp.cleanup)
        self.db = Path(temp.name) / "verified.sqlite"
        ledger(self.db, [
            (A, 1, 1, periodic(1_700_000_100)),
            (A, 2, 1, periodic(1_700_000_900)),          # newest measurement of A
            (B, 1, 1, periodic(1_700_000_500)),
            (A, 3, 1, periodic(1_700_000_300)),          # old backlog, arrived last
            (B, 2, 1, periodic(1_700_000_999, located=False)),  # no location
            (B, 3, 2, bytes(48)),                        # not a PERIODIC
        ])

    def test_newest_measurement_per_tracker_not_newest_arrival(self) -> None:
        got = read_latest(self.db)
        self.assertTrue(got["latest"])
        by_device = {r["device"]: r["sequence"] for r in got["records"]}
        self.assertEqual(by_device, {A.hex().upper(): 2, B.hex().upper(): 1})
        # Same record shape as history pages, so the phone stores it the same way.
        page = read_page(self.db, 0)["records"]
        self.assertIn(got["records"][0], page)

    def test_only_post_cutover_reports_can_update_last_heard(self) -> None:
        def put(seq: int, timestamp: int, located: bool) -> None:
            with sqlite3.connect(self.db) as db:
                db.execute(
                    "INSERT INTO verified_observations VALUES (?,?,?,?,?,?)",
                    (A, INC, seq, 1, periodic(timestamp, located), bytes([seq]) * 100),
                )
                db.execute(
                    "INSERT INTO edge_receipts VALUES (?,?,?,?,?,?)",
                    (A, INC, seq, 1800000000 + seq, -90, 8),
                )

        # A later-arriving OLD no-fix does not become a 'live' observation.
        put(4, LIVE_REPORT_CUTOVER_UTC - 1, False)
        by_device = {}
        for r in read_latest(self.db)["records"]:
            by_device.setdefault(r["device"], []).append(r["sequence"])
        self.assertEqual(by_device[A.hex().upper()], [2])

        # A measured post-cutover no-fix is a fresh REPORT, not a new GPS fix.
        put(5, LIVE_REPORT_CUTOVER_UTC + 120, False)
        by_device = {}
        for r in read_latest(self.db)["records"]:
            by_device.setdefault(r["device"], []).append(r["sequence"])
        self.assertEqual(by_device[A.hex().upper()], [2, 5])
        self.assertEqual(by_device[B.hex().upper()], [1])

        # Backlog arriving afterward cannot hide a new valid location.
        put(6, LIVE_REPORT_CUTOVER_UTC - 400, True)
        put(7, LIVE_REPORT_CUTOVER_UTC + 180, True)
        by_device = {}
        for r in read_latest(self.db)["records"]:
            by_device.setdefault(r["device"], []).append(r["sequence"])
        self.assertEqual(by_device[A.hex().upper()], [7])
        self.assertEqual(by_device[B.hex().upper()], [1])

    def test_reboot_at_cutover_and_delayed_previous_incarnation(self) -> None:
        from datetime import datetime
        boundary = int(datetime.fromisoformat("2026-11-20T20:41:00+03:00").timestamp())
        new_inc = bytes.fromhex("0000000000000001")
        with sqlite3.connect(self.db) as db:
            db.executemany("INSERT INTO verified_observations VALUES (?,?,?,?,?,?)", [
                (A, new_inc, 1, 1, periodic(boundary, False), bytes(100)),
                (A, INC, 99, 1, periodic(boundary - 1), bytes(100)),
            ])
        records = read_latest(self.db)["records"]
        self.assertEqual([(r["incarnation"], r["sequence"]) for r in records
                          if r["device"] == A.hex().upper()],
                         [(INC.hex().upper(), 99), (new_inc.hex().upper(), 1)])

    def test_startup_indexes_only_empty_ledgers(self) -> None:
        from backend.orun_backend_bench import Backend
        def indexes(path):
            with sqlite3.connect(path) as db:
                return {row[0] for row in db.execute(
                    "SELECT name FROM sqlite_master WHERE type='index' AND name LIKE 'latest_%'")}
        empty = self.db.with_name("empty.sqlite")
        Backend(empty, False)
        self.assertEqual(indexes(empty), {"latest_location_utc", "latest_report_utc"})
        before = read_latest(self.db)
        Backend(self.db, False)
        self.assertEqual(indexes(self.db), set())
        self.assertEqual(read_latest(self.db), before)

    def test_clockless_report_restores_gateway_last_heard_without_new_gps(self) -> None:
        with sqlite3.connect(self.db) as db:
            db.execute(
                "INSERT INTO verified_observations VALUES (?,?,?,?,?,?)",
                (A, INC, 9, 1, periodic(0, False), bytes([9]) * 100),
            )
            db.execute(
                "INSERT INTO edge_receipts VALUES (?,?,?,?,?,?)",
                (A, INC, 9, 1800000009, -88, 7),
            )
        a = [r for r in read_latest(self.db)["records"]
             if r["device"] == A.hex().upper()]
        self.assertEqual([r["sequence"] for r in a], [2, 9])
        self.assertEqual(a[1]["first_seen_utc"], 1800000009)
        self.assertEqual(a[0]["sequence"], 2)  # last valid GPS preserved

    def test_old_or_missing_clockless_receipts_do_not_claim_recent_contact(self) -> None:
        with sqlite3.connect(self.db) as db:
            db.executemany(
                "INSERT INTO verified_observations VALUES (?,?,?,?,?,?)",
                [(A, INC, 10, 1, periodic(0, False), bytes([10]) * 100),
                 (A, INC, 11, 1, periodic(0, False), bytes([11]) * 100)],
            )
            db.execute(
                "INSERT INTO edge_receipts VALUES (?,?,?,?,?,?)",
                (A, INC, 10, LIVE_REPORT_CUTOVER_UTC - 1, -88, 7),
            )
        a = [r for r in read_latest(self.db)["records"]
             if r["device"] == A.hex().upper()]
        self.assertEqual([r["sequence"] for r in a], [2])


    def test_http_route_bearer_and_exact_query(self) -> None:
        server = make_readonly_server(self.db, TOKEN)
        server.daemon_threads = True
        Thread(target=server.serve_forever, daemon=True).start()
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)

        def get(route: str, bearer: str | None):
            conn = http.client.HTTPConnection("127.0.0.1", server.server_port, timeout=4)
            try:
                conn.request("GET", route, headers={"Authorization": "Bearer " + bearer} if bearer else {})
                reply = conn.getresponse()
                return reply.status, json.loads(reply.read())
            finally:
                conn.close()

        self.assertEqual(get("/v1/edge/records?latest=1", None)[0], 401)
        status, body = get("/v1/edge/records?latest=1", TOKEN)
        self.assertEqual(status, 200)
        self.assertEqual(len(body["records"]), 2)
        self.assertEqual(get("/v1/edge/records?latest=2", TOKEN)[0], 400)
        self.assertEqual(get("/v1/edge/records?latest=1&after=0", TOKEN)[0], 400)

    def test_relay_accepts_only_exact_latest_query(self) -> None:
        relay = AccountObservationRelay.__new__(AccountObservationRelay)
        relay.observation_site = "orun-main"
        relay.records_host, relay.records_port = "127.0.0.1", 9   # unreachable on purpose
        relay.host, relay.port = "127.0.0.1", 9
        relay._token = "x" * 32
        for bad in ("/v1/edge/records?latest=2", "/v1/edge/records?latest=1&x=1"):
            with self.assertRaisesRegex(RelayUnavailable, "UNSUPPORTED_ROUTE"):
                relay.forward("GET", bad)
        # Accepted route reaches the network step (and fails there, not at the route check).
        with self.assertRaisesRegex(RelayUnavailable, "OBSERVATION_BACKEND_UNAVAILABLE"):
            relay.forward("GET", "/v1/edge/records?latest=1")


if __name__ == "__main__":
    unittest.main()
