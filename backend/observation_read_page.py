"""Read-only authenticated observation feed source for a single owner site.

The HTTP handler and site-scoped account relay provide the authorization.
This module reads only the already-verified, durable server ledger and returns
the FIRST exact protected frame. The Android client must independently verify
AEAD authenticity with its read key before adding a record to its SQLite.

The monotonically increasing SQLite rowid is a cursor, not a measurement time.
No observation, receipt, credential or custody state is modified here.
"""
from __future__ import annotations

from pathlib import Path
import sqlite3

PAGE_SIZE = 64
MAX_CURSOR = (1 << 63) - 1


class ObservationReadUnavailable(RuntimeError):
    pass


def read_page(db_path: str | Path, after: int) -> dict:
    if type(after) is not int or not 0 <= after <= MAX_CURSOR:
        raise ValueError("INVALID_OBSERVATION_CURSOR")
    path = Path(db_path)
    if not path.is_file():
        raise ObservationReadUnavailable("BACKEND_LEDGER_NOT_FOUND")
    uri = path.resolve().as_uri() + "?mode=ro"
    try:
        with sqlite3.connect(uri, uri=True, timeout=2.0) as db:
            db.execute("PRAGMA query_only=ON")
            rows = db.execute(
                "SELECT o.rowid, o.device_id, o.incarnation, o.sequence, "
                "o.family, o.exact_first_frame, r.first_seen_utc, r.gw_rssi, r.gw_snr "
                "FROM verified_observations o "
                "LEFT JOIN edge_receipts r USING (device_id, incarnation, sequence) "
                "WHERE o.rowid > ? ORDER BY o.rowid ASC LIMIT ?",
                (after, PAGE_SIZE + 1),
            ).fetchall()
    except sqlite3.Error as e:
        raise ObservationReadUnavailable("BACKEND_LEDGER_READ_FAILED") from e
    more = len(rows) > PAGE_SIZE
    selected = rows[:PAGE_SIZE]
    records = []
    for row in selected:
        cursor, dev, incarnation, seq, family, frame, seen, rssi, snr = row
        # Fail closed on corrupt or inconsistent database values.
        if (type(cursor) is not int or cursor <= after or
                not isinstance(dev, bytes) or len(dev) != 8 or
                not isinstance(incarnation, bytes) or len(incarnation) != 8 or
                type(seq) is not int or seq <= 0 or
                family not in (1, 2) or
                not isinstance(frame, bytes) or len(frame) not in (84, 100) or
                (seen is not None and (type(seen) is not int or seen <= 0)) or
                (rssi is not None and (type(rssi) is not int or not -200 <= rssi <= 50)) or
                (snr is not None and (type(snr) is not int or not -50 <= snr <= 50))):
            raise ObservationReadUnavailable("BACKEND_LEDGER_CORRUPT")
        records.append({
            "cursor": cursor, "device": dev.hex().upper(),
            "incarnation": incarnation.hex().upper(),
            "sequence": seq, "family": family,
            "frame": frame.hex().upper(),
            "first_seen_utc": seen, "rssi": rssi, "snr": snr,
        })
        after = cursor
    return {"records": records, "cursor": after, "has_more": more}
