"""3a: hangi tasmada hangi alanlar var (hesapta).

Uygulama alanları USB ile tasmaya yazıp tasmanın onayını ve yeniden okunan
durumu doğruladıktan sonra burada kaydeder. Telefondaki alanlar değişince
uygulama özeti (digest) karşılaştırıp "tasmadaki alanlar eski" der.
"""
from __future__ import annotations

from contextlib import closing
import json
from pathlib import Path
import re
import sqlite3
import time
from typing import Callable

SCHEMA = ("CREATE TABLE IF NOT EXISTS tracker_fences ("
          " device TEXT PRIMARY KEY, digest TEXT NOT NULL, areas INTEGER NOT NULL,"
          " vertices INTEGER NOT NULL, zones TEXT NOT NULL, applied_utc INTEGER NOT NULL)")
ID16 = re.compile(r"[0-9A-F]{16}")
DIGEST = re.compile(r"[0-9a-f]{16}")


class BadFence(ValueError):
    pass


def clean(body: object) -> tuple[str, str, int, int, list[dict]]:
    if not isinstance(body, dict):
        raise BadFence("body")
    device, digest = body.get("device"), body.get("digest")
    areas, vertices, zones = body.get("areas"), body.get("vertices"), body.get("zones")
    if not isinstance(device, str) or not ID16.fullmatch(device.upper()):
        raise BadFence("device")
    if not isinstance(digest, str) or not DIGEST.fullmatch(digest):
        raise BadFence("digest")
    for v, hi in ((areas, 8), (vertices, 64)):
        if not isinstance(v, int) or isinstance(v, bool) or not 1 <= v <= hi:
            raise BadFence("count")
    if not isinstance(zones, list) or len(zones) != areas:
        raise BadFence("zones")
    out = []
    for z in zones:
        if (not isinstance(z, dict) or not isinstance(z.get("id"), str) or not isinstance(z.get("name"), str)
                or not 1 <= len(z["id"]) <= 128 or len(z["name"]) > 80):
            raise BadFence("zones")
        out.append({"id": z["id"], "name": z["name"]})
    return device.upper(), digest, areas, vertices, out


class TrackerFences:
    def __init__(self, db_path: str | Path, clock: Callable[[], float] = time.time):
        self.db_path = str(db_path)
        self.clock = clock
        with closing(sqlite3.connect(self.db_path, timeout=2.0)) as db, db:
            db.execute(SCHEMA)

    def record(self, body: object) -> dict:
        device, digest, areas, vertices, zones = clean(body)
        now = int(self.clock())
        with closing(sqlite3.connect(self.db_path, timeout=2.0)) as db, db:
            db.execute("INSERT INTO tracker_fences VALUES (?,?,?,?,?,?) ON CONFLICT(device) DO UPDATE SET "
                       "digest=excluded.digest, areas=excluded.areas, vertices=excluded.vertices, "
                       "zones=excluded.zones, applied_utc=excluded.applied_utc",
                       (device, digest, areas, vertices, json.dumps(zones, ensure_ascii=False), now))
        return {"device": device, "applied_utc": now}

    def all(self) -> dict[str, dict]:
        with closing(sqlite3.connect(self.db_path, timeout=2.0)) as db:
            rows = db.execute("SELECT device,digest,areas,vertices,zones,applied_utc FROM tracker_fences "
                              "ORDER BY device LIMIT 2048").fetchall()
        return {r[0]: {"digest": r[1], "areas": r[2], "vertices": r[3],
                       "zones": json.loads(r[4]), "applied_utc": r[5]} for r in rows}
