"""4b: cihaz ayarları hesapta (sahip kararı 2026-11-02).

Her cihaz için iki şey saklanır:
  istenen  - kullanıcının uygulamada verdiği ayarlar (tek cihaza ya da toplu),
  cihazda  - cihazdan en son okunan değerler (USB/Bluetooth, ileride LoRa).
İkisi farklıysa ayar "bekliyor"dur; uygulama cihaza bağlanınca sormadan yazar.

Ayar adları bütün ORUN cihazları için tek listedir; bir cihaz yalnız kendinde
olan ayarları kullanır. Sensör/vana ayarları gelince bu listeye eklenir.
"""
from __future__ import annotations

from contextlib import closing
import json
from pathlib import Path
import re
import sqlite3
import time
from typing import Callable

SCHEMA = ("CREATE TABLE IF NOT EXISTS device_settings ("
          " device TEXT PRIMARY KEY, wanted TEXT NOT NULL DEFAULT '{}', wanted_utc INTEGER,"
          " reported TEXT, reported_utc INTEGER, reported_via TEXT)")
ID16 = re.compile(r"[0-9A-F]{16}")
MAX_DEVICES = 2048
MAX_BATCH = 256
VIAS = ("usb", "ble", "lora")


def _interval(v: object) -> bool:
    return isinstance(v, int) and not isinstance(v, bool) and 60 <= v <= 12 * 24 * 3600


def _flag(v: object) -> bool:
    return isinstance(v, bool)


# Bütün cihazlar için tek ayar listesi: ad -> doğrulama.
SETTINGS: dict[str, Callable[[object], bool]] = {
    "interval_s": _interval,   # rapor aralığı (saniye), tasmanın sınırları
    "tracking": _flag,         # konum takibi hizmeti
    "relay": _flag,            # aktarma hizmeti
}


class BadSettings(ValueError):
    pass


def clean_values(values: object) -> dict:
    if not isinstance(values, dict) or not values:
        raise BadSettings("values")
    for k, v in values.items():
        check = SETTINGS.get(k)
        if check is None or not check(v):
            raise BadSettings(k)
    return dict(values)


def clean_device(device: object) -> str:
    if not isinstance(device, str) or not ID16.fullmatch(device.upper()):
        raise BadSettings("device")
    return device.upper()


class DeviceSettings:
    def __init__(self, db_path: str | Path, clock: Callable[[], float] = time.time):
        self.db_path = str(db_path)
        self.clock = clock
        with closing(sqlite3.connect(self.db_path, timeout=2.0)) as db, db:
            db.execute(SCHEMA)

    def _ensure(self, db: sqlite3.Connection, devices: list[str]) -> None:
        count = db.execute("SELECT count(*) FROM device_settings").fetchone()[0]
        known = {r[0] for r in db.execute(
            "SELECT device FROM device_settings WHERE device IN (%s)" % ",".join("?" * len(devices)), devices)}
        if count + len(set(devices) - known) > MAX_DEVICES:
            raise BadSettings("too_many")

    def want(self, body: object) -> dict:
        """{"devices": [...], "set": {...}}: merge into each device's wanted settings."""
        if not isinstance(body, dict):
            raise BadSettings("body")
        devices = body.get("devices")
        if not isinstance(devices, list) or not 1 <= len(devices) <= MAX_BATCH:
            raise BadSettings("devices")
        ids = sorted({clean_device(d) for d in devices})
        values = clean_values(body.get("set"))
        now = int(self.clock())
        with closing(sqlite3.connect(self.db_path, timeout=2.0)) as db, db:
            self._ensure(db, ids)
            for dev in ids:
                row = db.execute("SELECT wanted FROM device_settings WHERE device=?", (dev,)).fetchone()
                wanted = {**(json.loads(row[0]) if row else {}), **values}
                db.execute("INSERT INTO device_settings(device,wanted,wanted_utc) VALUES (?,?,?) "
                           "ON CONFLICT(device) DO UPDATE SET wanted=excluded.wanted, wanted_utc=excluded.wanted_utc",
                           (dev, json.dumps(wanted, sort_keys=True), now))
        return {"devices": ids, "wanted_utc": now}

    def report(self, body: object) -> dict:
        """{"device", "values", "via"}: what the device itself said it has."""
        if not isinstance(body, dict):
            raise BadSettings("body")
        dev = clean_device(body.get("device"))
        values = clean_values(body.get("values"))
        via = body.get("via")
        if via not in VIAS:
            raise BadSettings("via")
        now = int(self.clock())
        with closing(sqlite3.connect(self.db_path, timeout=2.0)) as db, db:
            self._ensure(db, [dev])
            db.execute("INSERT INTO device_settings(device,reported,reported_utc,reported_via) VALUES (?,?,?,?) "
                       "ON CONFLICT(device) DO UPDATE SET reported=excluded.reported, "
                       "reported_utc=excluded.reported_utc, reported_via=excluded.reported_via",
                       (dev, json.dumps(values, sort_keys=True), now, via))
        return {"device": dev, "reported_utc": now}

    def all(self) -> dict[str, dict]:
        with closing(sqlite3.connect(self.db_path, timeout=2.0)) as db:
            rows = db.execute("SELECT device,wanted,wanted_utc,reported,reported_utc,reported_via "
                              "FROM device_settings ORDER BY device LIMIT ?", (MAX_DEVICES,)).fetchall()
        return {r[0]: {"wanted": json.loads(r[1]), "wanted_utc": r[2],
                       "reported": json.loads(r[3]) if r[3] else None, "reported_utc": r[4],
                       "reported_via": r[5]} for r in rows}
