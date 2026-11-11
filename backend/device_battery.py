"""Y3: tasmanın pil kapasitesi (mAh) hesapta (sahip kararı 2026-11-11).

Kapasiteyi tasma kullanmaz; yalnız uygulama pil ömrünü hesaplamak için
kullanır. Bu yüzden cihaza yazılmaz, adı gibi hesapta durur ve hesabın
bütün telefonlarında aynı görünür. Boş değer (None) kaydı siler.
"""
from __future__ import annotations

from contextlib import closing
from pathlib import Path
import re
import sqlite3
import time
from typing import Callable

SCHEMA = ("CREATE TABLE IF NOT EXISTS device_battery ("
          " device TEXT PRIMARY KEY, mah INTEGER NOT NULL, updated_utc INTEGER NOT NULL)")
ID16 = re.compile(r"[0-9A-F]{16}")
MAH_MIN, MAH_MAX = 50, 50_000
MAX_DEVICES = 2048


class BadBattery(ValueError):
    pass


def clean(device: object, mah: object) -> tuple[str, int | None]:
    if not isinstance(device, str) or not ID16.fullmatch(device.upper()):
        raise BadBattery("device")
    if mah is None:
        return device.upper(), None
    if isinstance(mah, bool) or not isinstance(mah, int) or not MAH_MIN <= mah <= MAH_MAX:
        raise BadBattery("mah")
    return device.upper(), mah


class DeviceBattery:
    def __init__(self, db_path: str | Path, clock: Callable[[], float] = time.time):
        self.db_path = str(db_path)
        self.clock = clock
        with closing(sqlite3.connect(self.db_path, timeout=2.0)) as db, db:
            db.execute(SCHEMA)

    def all(self) -> dict[str, int]:
        with closing(sqlite3.connect(self.db_path, timeout=2.0)) as db:
            return {r[0]: r[1] for r in db.execute(
                "SELECT device,mah FROM device_battery ORDER BY device LIMIT ?", (MAX_DEVICES,))}

    def set(self, device: object, mah: object) -> tuple[str, int | None]:
        dev, value = clean(device, mah)
        with closing(sqlite3.connect(self.db_path, timeout=2.0)) as db, db:
            if value is None:
                db.execute("DELETE FROM device_battery WHERE device=?", (dev,))
            else:
                count = db.execute("SELECT count(*) FROM device_battery").fetchone()[0]
                exists = db.execute("SELECT 1 FROM device_battery WHERE device=?", (dev,)).fetchone()
                if not exists and count >= MAX_DEVICES:
                    raise BadBattery("too_many")
                db.execute("INSERT INTO device_battery VALUES (?,?,?) ON CONFLICT(device) DO UPDATE SET "
                           "mah=excluded.mah, updated_utc=excluded.updated_utc",
                           (dev, value, int(self.clock())))
        return dev, value
