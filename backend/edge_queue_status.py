"""Last reported PC Edge SQLite inbox counters.

These are observations of the sender's local sealed-record spool. A delivered
count is a PC-local backend ACK, not a unique farm-wide record count.
Only authenticated bridge reports can write; owner account read is gated by
the existing site-scoped zone:read route. All timestamps are SERVER times.
"""
from __future__ import annotations

from contextlib import closing
from pathlib import Path
import re
import sqlite3
import time
from typing import Callable

SCHEMA = """
CREATE TABLE IF NOT EXISTS pc_edge_queue_status (
 edge TEXT PRIMARY KEY,
 pending INTEGER NOT NULL,
 delivered INTEGER NOT NULL,
 quarantined INTEGER NOT NULL,
 seen_utc INTEGER NOT NULL
)
"""
EDGE_ID = re.compile(r"^[A-Za-z0-9._-]{1,64}$")
MAX_COUNT = 1_000_000_000


class BadEdgeQueueStatus(ValueError):
    pass


def parse_edge_queue_report(body: object) -> tuple[str, int, int, int]:
    if not isinstance(body, dict) or set(body) != {"edge", "pending", "delivered", "quarantined"}:
        raise BadEdgeQueueStatus("shape")
    edge = body["edge"]
    if not isinstance(edge, str) or not EDGE_ID.fullmatch(edge):
        raise BadEdgeQueueStatus("edge")
    counts = (body["pending"], body["delivered"], body["quarantined"])
    if any(type(x) is not int or not 0 <= x <= MAX_COUNT for x in counts):
        raise BadEdgeQueueStatus("counts")
    return edge, *counts


class EdgeQueueStatus:
    def __init__(self, db_path: str | Path, clock: Callable[[], float] = time.time):
        self.db_path = str(db_path)
        self.clock = clock
        with closing(sqlite3.connect(self.db_path, timeout=2)) as db, db:
            db.execute(SCHEMA)

    def report(self, edge: str, pending: int, delivered: int, quarantined: int) -> None:
        edge, pending, delivered, quarantined = parse_edge_queue_report({
            "edge": edge, "pending": pending, "delivered": delivered, "quarantined": quarantined
        })
        with closing(sqlite3.connect(self.db_path, timeout=2)) as db, db:
            db.execute(
                "INSERT INTO pc_edge_queue_status VALUES (?,?,?,?,?) "
                "ON CONFLICT(edge) DO UPDATE SET "
                "pending=excluded.pending, delivered=excluded.delivered, "
                "quarantined=excluded.quarantined, seen_utc=excluded.seen_utc",
                (edge, pending, delivered, quarantined, int(self.clock())))

    def list(self) -> list[dict]:
        with closing(sqlite3.connect(self.db_path, timeout=2)) as db:
            rows = db.execute(
                "SELECT edge,pending,delivered,quarantined,seen_utc "
                "FROM pc_edge_queue_status ORDER BY edge LIMIT 256").fetchall()
        return [dict(zip(("edge", "pending", "delivered", "quarantined", "seen_utc"), row))
                for row in rows]
