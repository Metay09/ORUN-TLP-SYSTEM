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
# Upper bound on trackers returned by read_latest (one record each).
LATEST_MAX = 64

_RECORD_COLUMNS = (
    "SELECT o.rowid, o.device_id, o.incarnation, o.sequence, "
    "o.family, o.exact_first_frame, r.first_seen_utc, r.gw_rssi, r.gw_snr "
    "FROM verified_observations o "
    "LEFT JOIN edge_receipts r USING (device_id, incarnation, sequence) ")


class ObservationReadUnavailable(RuntimeError):
    pass


def _open(db_path: str | Path) -> sqlite3.Connection:
    path = Path(db_path)
    if not path.is_file():
        raise ObservationReadUnavailable("BACKEND_LEDGER_NOT_FOUND")
    uri = path.resolve().as_uri() + "?mode=ro"
    return sqlite3.connect(uri, uri=True, timeout=2.0)


def _record(row: tuple, after: int) -> dict:
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
    return {
        "cursor": cursor, "device": dev.hex().upper(),
        "incarnation": incarnation.hex().upper(),
        "sequence": seq, "family": family,
        "frame": frame.hex().upper(),
        "first_seen_utc": seen, "rssi": rssi, "snr": snr,
    }


def read_page(db_path: str | Path, after: int) -> dict:
    if type(after) is not int or not 0 <= after <= MAX_CURSOR:
        raise ValueError("INVALID_OBSERVATION_CURSOR")
    try:
        with _open(db_path) as db:
            db.execute("PRAGMA query_only=ON")
            rows = db.execute(
                _RECORD_COLUMNS + "WHERE o.rowid > ? ORDER BY o.rowid ASC LIMIT ?",
                (after, PAGE_SIZE + 1),
            ).fetchall()
    except sqlite3.Error as e:
        raise ObservationReadUnavailable("BACKEND_LEDGER_READ_FAILED") from e
    more = len(rows) > PAGE_SIZE
    records = []
    for row in rows[:PAGE_SIZE]:
        records.append(_record(row, after))
        after = records[-1]["cursor"]
    return {"records": records, "cursor": after, "has_more": more}


def _measured_utc(plaintext: object) -> int | None:
    """GNSS measurement time of a PERIODIC plaintext with a valid location.

    SF5B PERIODIC (64 bytes): flags at [1] (bit0 time valid, bit1 location
    valid), measurement UTC seconds big-endian at [16:20]. Arrival order is
    NOT measurement order (store-and-forward), so "latest" uses this field.
    """
    if not isinstance(plaintext, bytes) or len(plaintext) != 64:
        return None
    if plaintext[1] & 0b11 != 0b11:
        return None
    t = int.from_bytes(plaintext[16:20], "big")
    return t if t > 0 else None


def read_latest(db_path: str | Path) -> dict:
    """For each tracker, the exact protected frame of its newest MEASURED
    position (not the newest arrival). Same record shape as read_page; the
    phone verifies AEAD and stores it without moving its history cursor."""
    try:
        with _open(db_path) as db:
            db.execute("PRAGMA query_only=ON")
            best: dict[bytes, tuple[int, int]] = {}
            for rowid, dev, plaintext in db.execute(
                    "SELECT rowid, device_id, plaintext FROM verified_observations "
                    "WHERE family = 1"):
                t = _measured_utc(plaintext)
                if t is None or not isinstance(dev, bytes):
                    continue
                if dev not in best or (t, rowid) > best[dev]:
                    best[dev] = (t, rowid)
            winners = sorted(best.items(), key=lambda kv: kv[1][0], reverse=True)[:LATEST_MAX]
            records = []
            for _dev, (_t, rowid) in winners:
                row = db.execute(_RECORD_COLUMNS + "WHERE o.rowid = ?", (rowid,)).fetchone()
                if row is None:
                    raise ObservationReadUnavailable("BACKEND_LEDGER_CHANGED")
                records.append(_record(row, 0))
    except sqlite3.Error as e:
        raise ObservationReadUnavailable("BACKEND_LEDGER_READ_FAILED") from e
    return {"records": records, "latest": True}
