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
# Reports with GNSS time use their measured time; clockless no-fix reports
# use only a verified Gateway's first receipt after the pilot cutover.
# Historical receipts retain their original time, never freshened by upload.
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


# Four big-endian bytes compare in UTC order. The low flag nibble contains
# the time (bit 0) and location (bit 1) validity flags.
_STAMP = "substr(plaintext,17,4)"
def _periodic_where(nibbles: str) -> str:
    values = ",".join(f"'{n}'" for n in nibbles)
    return ("family=1 AND typeof(plaintext)='blob' AND length(plaintext)=64 "
            f"AND substr(hex(substr(plaintext,2,1)),2,1) IN ({values})")


_LOCATION = _periodic_where("37BF")
_REPORT = _periodic_where("13579BDF")
LATEST_INDEXES = tuple(
    f"CREATE INDEX IF NOT EXISTS latest_{name}_utc ON verified_observations "
    f"(device_id,{_STAMP}) WHERE {predicate}"
    for name, predicate in (("location", _LOCATION), ("report", _REPORT))
)


def read_latest(db_path: str | Path) -> dict:
    """Latest measured POSITION and latest evidenced REPORT per tracker.

    A clockless no-fix report with post-cutover first_seen_utc proves when the
    Gateway first heard the collar, NOT when GPS measured a position. The old
    last-known position remains available independently. History cursors and
    custody state are unchanged; upload time cannot refresh an old receipt.
    At most two records per tracker, and at most LATEST_MAX trackers.
    """
    try:
        with _open(db_path) as db:
            db.execute("PRAGMA query_only=ON")
            locations: dict[bytes, tuple[int, int]] = {}
            reports: dict[bytes, tuple[int, int]] = {}
            db.execute("BEGIN")
            # Seek past each device using the primary key, then reverse-seek
            # each measured-time index. Older read-only ledgers also work.
            dev = b""
            while True:
                found = db.execute(
                    "SELECT device_id FROM verified_observations WHERE device_id>? "
                    "ORDER BY device_id LIMIT 1", (dev,)).fetchone()
                if found is None:
                    break
                dev = found[0]
                for predicate, minimum, target in (
                        (_LOCATION, 1, locations),
                        (_REPORT, LIVE_REPORT_CUTOVER_UTC, reports)):
                    row = db.execute(
                        f"SELECT {_STAMP},rowid FROM verified_observations "
                        f"WHERE device_id=? AND {predicate} AND {_STAMP}>=? "
                        f"ORDER BY {_STAMP} DESC,rowid DESC LIMIT 1",
                        (dev, minimum.to_bytes(4, "big"))).fetchone()
                    if row:
                        target[dev] = (int.from_bytes(row[0], "big"), row[1])

                # A clockless no-fix PERIODIC still proves that the gateway heard
                # the tracker at first_seen_utc. This is receipt time, NOT a GNSS
                # measurement; never fabricate one or replace the last GPS fix.
                # The original receipt time is stable across delayed uploads and
                # duplicate/replayed history. Do not use pre-cutover receipts.
                clockless = db.execute(
                    "SELECT r.first_seen_utc,o.rowid "
                    "FROM verified_observations o "
                    "JOIN edge_receipts r USING (device_id,incarnation,sequence) "
                    "WHERE o.device_id=? AND o.family=1 "
                    "AND typeof(o.plaintext)='blob' AND length(o.plaintext)=64 "
                    "AND substr(hex(substr(o.plaintext,2,1)),2,1) IN ('0','4','8','C') "
                    "AND r.first_seen_utc>=? "
                    "ORDER BY r.first_seen_utc DESC,o.rowid DESC LIMIT 1",
                    (dev, LIVE_REPORT_CUTOVER_UTC),
                ).fetchone()
                if clockless and (dev not in reports or clockless > reports[dev]):
                    reports[dev] = clockless

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
