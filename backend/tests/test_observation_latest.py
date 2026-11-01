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
from backend.observation_read_page import read_latest, read_page
from tools.orun_observation_readonly_sidecar import make_readonly_server

A = bytes.fromhex("52E16773141A8887")
B = bytes.fromhex("0011223344556677")
INC = bytes.fromhex("5523010CEA37319F")
TOKEN = "SYNTHETIC-LATEST-TEST-BEARER-123456"


def periodic(measured: int, located: bool = True) -> bytes:
    p = bytearray(64)
    p[1] = 0b11 if located else 0b01
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
