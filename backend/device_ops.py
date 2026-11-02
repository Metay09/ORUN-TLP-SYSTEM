"""İşlemler (sahip kararı 2026-11-02): alan ve ayar gönderme kaydı, hesapta.

Bir işlem (op) kullanıcının "uygula/kaydet" dediği andır; her hedef cihaz için
ayrı bir satır tutulur. Durumlar:
  waiting     sırada: cihaza henüz işlenmedi
  gateway     gateway'de (yalnız LoRa yolu; gateway'in zaten gönderdiği
              bilgiden, fazladan telsiz trafiği olmadan)
  applied     tasmaya işlendi: YALNIZ cihazın kendi onayıyla (USB/BLE sonuç
              satırı, LoRa'da tasmanın onayı), yol ne olursa olsun
  same        cihaz "zaten bu" dedi
  rejected    cihaz reddetti
  superseded  aynı cihaza aynı türden daha yeni işlem geldi
Yeni bir işlem, aynı cihazdaki aynı türden bekleyen eski işlemin yerine geçer.
"""
from __future__ import annotations

from contextlib import closing
import json
from pathlib import Path
import re
import sqlite3
import time
from typing import Callable

SCHEMA = ("CREATE TABLE IF NOT EXISTS device_ops ("
          " op TEXT NOT NULL, device TEXT NOT NULL, kind TEXT NOT NULL, payload TEXT NOT NULL,"
          " state TEXT NOT NULL, created_utc INTEGER NOT NULL, updated_utc INTEGER NOT NULL,"
          " via TEXT, note TEXT, PRIMARY KEY (op, device))")
ID16 = re.compile(r"[0-9A-F]{16}")
OP_ID = re.compile(r"op-[0-9a-z]{4,32}")
KINDS = ("fence", "settings")
STATES = ("waiting", "gateway", "applied", "same", "rejected", "superseded")
RESULTS = ("gateway", "applied", "same", "rejected")
VIAS = ("usb", "ble", "lora")
MAX_ROWS = 4000
MAX_TARGETS = 256
MAX_PAYLOAD = 8192


class BadOp(ValueError):
    pass


def _device(d: object) -> str:
    if not isinstance(d, str) or not ID16.fullmatch(d.upper()):
        raise BadOp("device")
    return d.upper()


def _op(o: object) -> str:
    if not isinstance(o, str) or not OP_ID.fullmatch(o):
        raise BadOp("op")
    return o


class DeviceOps:
    def __init__(self, db_path: str | Path, clock: Callable[[], float] = time.time):
        self.db_path = str(db_path)
        self.clock = clock
        with closing(sqlite3.connect(self.db_path, timeout=2.0)) as db, db:
            db.execute(SCHEMA)
            db.execute("CREATE INDEX IF NOT EXISTS device_ops_dev ON device_ops(device, kind, state)")

    def create(self, body: object) -> dict:
        """{"op", "kind", "items": [{"device", "payload"}]}; the same op twice is a no-op."""
        if not isinstance(body, dict):
            raise BadOp("body")
        op, kind, items = _op(body.get("op")), body.get("kind"), body.get("items")
        if kind not in KINDS:
            raise BadOp("kind")
        if not isinstance(items, list) or not 1 <= len(items) <= MAX_TARGETS:
            raise BadOp("items")
        rows = []
        for it in items:
            if not isinstance(it, dict) or not isinstance(it.get("payload"), dict):
                raise BadOp("item")
            text = json.dumps(it["payload"], sort_keys=True, ensure_ascii=False)
            if len(text) > MAX_PAYLOAD:
                raise BadOp("payload")
            rows.append((_device(it.get("device")), text))
        if len({d for d, _ in rows}) != len(rows):
            raise BadOp("items")
        now = int(self.clock())
        with closing(sqlite3.connect(self.db_path, timeout=2.0)) as db, db:
            if db.execute("SELECT 1 FROM device_ops WHERE op=? LIMIT 1", (op,)).fetchone():
                return {"op": op, "created": False}
            if db.execute("SELECT count(*) FROM device_ops").fetchone()[0] + len(rows) > MAX_ROWS:
                # oldest finished rows go first; waiting ones are never dropped
                db.execute("DELETE FROM device_ops WHERE rowid IN (SELECT rowid FROM device_ops "
                           "WHERE state != 'waiting' ORDER BY updated_utc LIMIT ?)", (len(rows),))
            for dev, text in rows:
                db.execute("UPDATE device_ops SET state='superseded', updated_utc=? "
                           "WHERE device=? AND kind=? AND state IN ('waiting','gateway')", (now, dev, kind))
                db.execute("INSERT INTO device_ops VALUES (?,?,?,?,?,?,?,NULL,NULL)",
                           (op, dev, kind, text, "waiting", now, now))
        return {"op": op, "created": True}

    def result(self, body: object) -> dict:
        """{"op", "device", "state", "via", "note"?}: what the device (or the gateway) said."""
        if not isinstance(body, dict):
            raise BadOp("body")
        op, dev, state, via = _op(body.get("op")), _device(body.get("device")), body.get("state"), body.get("via")
        note = body.get("note")
        if state not in RESULTS or via not in VIAS:
            raise BadOp("state")
        if state == "gateway" and via != "lora":
            raise BadOp("state")
        if note is not None and (not isinstance(note, str) or len(note) > 120):
            raise BadOp("note")
        now = int(self.clock())
        with closing(sqlite3.connect(self.db_path, timeout=2.0)) as db, db:
            row = db.execute("SELECT state FROM device_ops WHERE op=? AND device=?", (op, dev)).fetchone()
            if row is None:
                raise BadOp("unknown")
            # A device's own answer is final; a later "gateway" never goes back over it.
            if row[0] in ("applied", "same", "rejected") or (state == "gateway" and row[0] != "waiting"):
                return {"op": op, "device": dev, "state": row[0]}
            db.execute("UPDATE device_ops SET state=?, updated_utc=?, via=?, note=? WHERE op=? AND device=?",
                       (state, now, via, note, op, dev))
        return {"op": op, "device": dev, "state": state}

    def recent(self, limit: int = 500) -> list[dict]:
        with closing(sqlite3.connect(self.db_path, timeout=2.0)) as db:
            rows = db.execute("SELECT op,device,kind,payload,state,created_utc,updated_utc,via,note FROM device_ops "
                              "ORDER BY created_utc DESC, op, device LIMIT ?", (limit,)).fetchall()
        return [{"op": r[0], "device": r[1], "kind": r[2], "payload": json.loads(r[3]), "state": r[4],
                 "created_utc": r[5], "updated_utc": r[6], "via": r[7], "note": r[8]} for r in rows]
