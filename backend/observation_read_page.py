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
# Only reports measured after the 20 Nov 2026 pilot cutover are promoted
# to the live latest-report lane. Historical/no-clock backlogs stay untouched.
LIVE_REPORT_CUTOVER_UTC = 1795196460  # 2026-11-20 20:41 Europe/Istanbul

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


def _report_utc(plaintext: object) -> int | None:
    """Authenticated PERIODIC report time, even without a GNSS location.

    A no-clock record cannot prove freshness merely because it was delivered
    recently. It is intentionally excluded from the forward-only live lane.
    """
    if not isinstance(plaintext, bytes) or len(plaintext) != 64:
        return None
    if not plaintext[1] & 0b01:
        return None
    t = int.from_bytes(plaintext[16:20], "big")
    return t if t > 0 else None


def read_latest(db_path: str | Path) -> dict:
    """Latest measured POSITION and (only after cutover) latest REPORT.

    A post-cutover report without a GNSS fix proves that a collar was heard,
    without fabricating a new location. The old last-known position remains
    available independently. History cursors/custody and old records are not
    changed; an old report arriving late cannot supersede a newer measurement.
    At most two records per tracker, and at most LATEST_MAX trackers.
    """
    try:
        with _open(db_path) as db:
            db.execute("PRAGMA query_only=ON")
            locations: dict[bytes, tuple[int, int]] = {}
            reports: dict[bytes, tuple[int, int]] = {}
            for rowid, dev, plaintext in db.execute(
                    "SELECT rowid, device_id, plaintext FROM verified_observations "
                    "WHERE family = 1"):
                if not isinstance(dev, bytes):
                    continue
                t_pos = _measured_utc(plaintext)
                if t_pos is not None and (
                        dev not in locations or (t_pos, rowid) > locations[dev]):
                    locations[dev] = (t_pos, rowid)
                t_report = _report_utc(plaintext)
                if t_report is not None and t_report >= LIVE_REPORT_CUTOVER_UTC and (
                        dev not in reports or (t_report, rowid) > reports[dev]):
                    reports[dev] = (t_report, rowid)

            # A report timestamp is authoritative for freshness, not arrival
            # order. Keep the last valid location when the latest report has
            # no location, but never call an old backlog packet live.
            devices = sorted(
                locations.keys() | reports.keys(),
                key=lambda dev: max(locations.get(dev, (0, 0)),
                                    reports.get(dev, (0, 0)))[0],
                reverse=True,
            )[:LATEST_MAX]
            records = []
            for dev in devices:
                loc = locations.get(dev)
                report = reports.get(dev)
                ids: list[int] = []
                if loc is not None:
                    ids.append(loc[1])
                if report is not None and report[1] not in ids and (
                        loc is None or report > loc):
                    ids.append(report[1])
                for rowid in ids:
                    row = db.execute(
                        _RECORD_COLUMNS + "WHERE o.rowid = ?", (rowid,)
                    ).fetchone()
                    if row is None:
                        raise ObservationReadUnavailable("BACKEND_LEDGER_CHANGED")
                    records.append(_record(row, 0))
    except sqlite3.Error as e:
        raise ObservationReadUnavailable("BACKEND_LEDGER_READ_FAILED") from e
    return {"records": records, "latest": True}
