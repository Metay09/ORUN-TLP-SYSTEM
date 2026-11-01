"""2d: cihaz adları hesapta (sahip kararı 2026-11-01).

Tasmanın adı hayvanın adıdır; gateway'in adı kendi adıdır. Ad cihaza yazılmaz;
Bluetooth'ta cihaz yine ORUN-XXXXXXXX diye görünür, uygulama adıyla gösterir.
Boş ad, adı siler (uygulama ORUN-XXXXXXXX gösterir).
"""
from __future__ import annotations

from contextlib import closing
from pathlib import Path
import re
import sqlite3
import time
from typing import Callable

SCHEMA = ("CREATE TABLE IF NOT EXISTS device_names ("
          " device TEXT PRIMARY KEY, name TEXT NOT NULL, updated_utc INTEGER NOT NULL)")
ID16 = re.compile(r"[0-9A-F]{16}")
MAX_NAME = 40
MAX_DEVICES = 2048


class BadName(ValueError):
    pass


def clean(device: object, name: object) -> tuple[str, str]:
    if not isinstance(device, str) or not ID16.fullmatch(device.upper()):
        raise BadName("device")
    if not isinstance(name, str):
        raise BadName("name")
    text = " ".join(name.split())
    if len(text) > MAX_NAME or any(ord(c) < 32 or 0x7F <= ord(c) < 0xA0 for c in text):
        raise BadName("name")
    return device.upper(), text


class DeviceNames:
    def __init__(self, db_path: str | Path, clock: Callable[[], float] = time.time):
        self.db_path = str(db_path)
        self.clock = clock
        with closing(sqlite3.connect(self.db_path, timeout=2.0)) as db, db:
            db.execute(SCHEMA)

    def all(self) -> dict[str, str]:
        with closing(sqlite3.connect(self.db_path, timeout=2.0)) as db:
            return {r[0]: r[1] for r in db.execute(
                "SELECT device,name FROM device_names ORDER BY device LIMIT ?", (MAX_DEVICES,))}

    def set(self, device: object, name: object) -> tuple[str, str]:
        dev, text = clean(device, name)
        with closing(sqlite3.connect(self.db_path, timeout=2.0)) as db, db:
            if not text:
                db.execute("DELETE FROM device_names WHERE device=?", (dev,))
            else:
                count = db.execute("SELECT count(*) FROM device_names").fetchone()[0]
                exists = db.execute("SELECT 1 FROM device_names WHERE device=?", (dev,)).fetchone()
                if not exists and count >= MAX_DEVICES:
                    raise BadName("too_many")
                db.execute("INSERT INTO device_names VALUES (?,?,?) ON CONFLICT(device) DO UPDATE SET "
                           "name=excluded.name, updated_utc=excluded.updated_utc",
                           (dev, text, int(self.clock())))
        return dev, text
