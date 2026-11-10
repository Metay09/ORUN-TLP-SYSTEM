"""2c: çiftliğin gateway'leri — Bağlantı ekranındaki "Diğer gateway'ler".

Köprü (PC Edge ya da telefon) bağlı olduğu gateway'in durumunu iki dakikada
bir bildirir: kimlik, nasıl bağlı, bellek. "Kaç tasma duyuyor" son 24 saatte
o gateway'den gelen kayıtlardan (backend_uploads) sayılır.
"""
from __future__ import annotations

from contextlib import closing
from pathlib import Path
import re
import sqlite3
import time
from typing import Callable, Iterable

SCHEMA = ("CREATE TABLE IF NOT EXISTS gateway_status ("
          " gateway TEXT PRIMARY KEY, via TEXT NOT NULL,"
          " held INTEGER, capacity INTEGER, seen_utc INTEGER NOT NULL)")
VIAS = ("pc-usb", "pc-ble", "phone-usb", "phone-ble")
ID16 = re.compile(r"[0-9A-F]{16}")
LIVE_S = 300          # bu kadar süredir bildirim yoksa "bağlı değil"
DAY_S = 86_400


class BadStatus(ValueError):
    pass


def parse_report(body: object, prefix: str) -> tuple[str, str, int | None, int | None]:
    """{"gateway","transport":"usb"|"ble","held","capacity"} → (gateway, via, held, capacity)."""
    if not isinstance(body, dict):
        raise BadStatus("body")
    gateway, transport = body.get("gateway"), body.get("transport")
    if not isinstance(gateway, str) or not ID16.fullmatch(gateway.upper()):
        raise BadStatus("gateway")
    if transport not in ("usb", "ble"):
        raise BadStatus("transport")
    held, capacity = body.get("held"), body.get("capacity")
    for v in (held, capacity):
        if v is not None and (not isinstance(v, int) or isinstance(v, bool) or not 0 <= v <= 1_000_000):
            raise BadStatus("held")
    if held is not None and capacity is not None and held > capacity:
        raise BadStatus("held")
    return gateway.upper(), f"{prefix}-{transport}", held, capacity


class GatewayStatus:
    def __init__(self, db_path: str | Path, clock: Callable[[], float] = time.time):
        self.db_path = str(db_path)
        self.clock = clock
        with closing(sqlite3.connect(self.db_path, timeout=2.0)) as db, db:
            db.execute(SCHEMA)

    def report(self, gateway: str, via: str, held: int | None, capacity: int | None) -> None:
        if not ID16.fullmatch(gateway) or via not in VIAS:
            raise BadStatus("report")
        with closing(sqlite3.connect(self.db_path, timeout=2.0)) as db, db:
            db.execute(
                "INSERT INTO gateway_status VALUES (?,?,?,?,?) ON CONFLICT(gateway) DO UPDATE SET "
                "via=excluded.via, held=excluded.held, capacity=excluded.capacity, "
                "seen_utc=excluded.seen_utc",
                (gateway, via, held, capacity, int(self.clock())))

    def list(self, known: Iterable[str] = ()) -> list[dict]:
        now = int(self.clock())
        with closing(sqlite3.connect(self.db_path, timeout=2.0)) as db:
            status = {r[0]: r[1:] for r in db.execute(
                "SELECT gateway,via,held,capacity,seen_utc FROM gateway_status")}
            try:
                uploads = {r[0]: (r[1], r[2]) for r in db.execute(
                    "SELECT gateway, max(uploaded_utc), "
                    "count(DISTINCT CASE WHEN uploaded_utc>=? THEN device_id END) "
                    "FROM backend_uploads WHERE gateway IS NOT NULL GROUP BY gateway",
                    (now - DAY_S,))}
            except sqlite3.OperationalError:     # kayıt defteri henüz yok
                uploads = {}
        out = []
        for gw in sorted(set(known) | set(status) | set(uploads)):
            via, held, capacity, seen = status.get(gw, (None, None, None, None))
            last_upload, trackers = uploads.get(gw, (None, 0))
            last = max((t for t in (seen, last_upload) if t is not None), default=None)
            live = seen is not None and now - seen <= LIVE_S
            out.append({"gateway": gw, "via": via if live else None,
                        "held": held, "capacity": capacity,
                        "status_seen_utc": seen,  # actual gateway memory snapshot timestamp
                        "last_seen_utc": last, "last_upload_utc": last_upload,
                        "trackers_24h": trackers})
        out.sort(key=lambda g: -(g["last_seen_utc"] or 0))
        return out
