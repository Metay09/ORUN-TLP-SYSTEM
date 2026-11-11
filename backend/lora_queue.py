"""6c: LoRa'dan ayar komutları, sunucu tarafı (sahip kararı 2026-11-11).

Uygulamada bir ayar işlemi (device_ops, kind=settings) oluşunca sunucu o
tasma için imzalı bir LoRa komutu hazırlar (lora_command.build_command,
tasmanın yazma anahtarıyla) ve kuyruğa koyar. Köprüler (PC Edge, telefon)
kuyruğu alıp gateway'lerine verir; hangi gateway tasmayı duyarsa o gönderir
(birden çok gateway aynı komutu taşıyabilir, aynı komut ikinci kez aynı
cevabı alır). Köprü gateway'in söylediklerini geri bildirir:
  queued   gateway komutu aldı         -> işlem "Gateway'de"
  sent     gateway telsizden gönderdi
  dropped  gateway 6 raporda cevap alamadı (kuyrukta kalır, yine verilir)
  result   tasmanın imzalı cevabı      -> sunucu doğrular; "Tasmaya işlendi"
           yalnız tasmanın imzasıyla. STALE: tasma bu arada başka yoldan
           değişmiş; sunucu yeni duruma göre yeniden imzalar.

Komut tasmanın kayıtlı ayar durumuna (state token) bağlıdır. Durum bilinmiyorsa
önce bilerek eski durumla bir "yoklama" komutu gider; tasmanın STALE cevabı
güncel durumu taşır, gerçek komut ona göre imzalanır. Her cevap durumu
günceller, bir sonraki komut doğrudan gider.
"""
from __future__ import annotations

from contextlib import closing
import json
from pathlib import Path
import re
import secrets
import sqlite3
import time
from typing import Callable, Optional

from backend import lora_command as lc
from backend import orun_credentials as oc

SCHEMA_QUEUE = ("CREATE TABLE IF NOT EXISTS lora_queue ("
                " device TEXT PRIMARY KEY, op TEXT NOT NULL, entries TEXT NOT NULL,"
                " command_id INTEGER NOT NULL, frame TEXT NOT NULL, probe INTEGER NOT NULL,"
                " state TEXT NOT NULL, sent INTEGER NOT NULL DEFAULT 0,"
                " created_utc INTEGER NOT NULL, updated_utc INTEGER NOT NULL, note TEXT)")
SCHEMA_TOKEN = ("CREATE TABLE IF NOT EXISTS lora_token ("
                " device TEXT PRIMARY KEY, incarnation TEXT NOT NULL, revision INTEGER NOT NULL,"
                " updated_utc INTEGER NOT NULL)")
ID16 = re.compile(r"[0-9A-F]{16}")
HEX = re.compile(r"[0-9A-F]+")
KINDS = ("queued", "sent", "dropped", "result")
MAX_EVENTS = 32
SERVICE_TRACKING, SERVICE_RELAY = 0x01, 0x02
MODE_EXPLICIT = 1


class BadEvent(ValueError):
    pass


def entries_for(values: dict, reported: Optional[dict]) -> list[tuple[int, int]]:
    """Uygulamanın ayar adları -> tek ayar tablosu satırları (numara, değer)."""
    out: list[tuple[int, int]] = []
    if "interval_s" in values:
        out.append((1, int(values["interval_s"])))
    if "tracking" in values or "relay" in values:
        now = reported or {}
        tracking = values.get("tracking", now.get("tracking", True))
        relay = values.get("relay", now.get("relay", False))
        services = (SERVICE_TRACKING if tracking else 0) | (SERVICE_RELAY if relay else 0)
        out.append((2, MODE_EXPLICIT | (services << 8)))
    if "ble_window_s" in values:
        out.append((3, int(values["ble_window_s"])))
    return out


def values_from(r: lc.Result) -> dict:
    services = r.requested_services if r.service_mode == MODE_EXPLICIT else SERVICE_TRACKING
    return {"interval_s": r.interval_s, "ble_window_s": r.ble_window_s,
            "tracking": bool(services & SERVICE_TRACKING), "relay": bool(services & SERVICE_RELAY)}


class LoraQueue:
    def __init__(self, db_path: str | Path, registry: Callable[[], Optional[oc.CredentialRegistry]],
                 ops, settings, clock: Callable[[], float] = time.time):
        self.db_path = str(db_path)
        self.registry, self.ops, self.settings, self.clock = registry, ops, settings, clock
        with closing(sqlite3.connect(self.db_path, timeout=2.0)) as db, db:
            db.execute(SCHEMA_QUEUE)
            db.execute(SCHEMA_TOKEN)

    # -- keys and state ------------------------------------------------------
    def _cred(self, device: str):
        reg = self.registry()
        if reg is None:
            return None
        try:
            return reg.active(int(device, 16))
        except oc.RegistryError:
            return None

    def _token(self, db, device: str) -> Optional[tuple[int, int]]:
        row = db.execute("SELECT incarnation, revision FROM lora_token WHERE device=?", (device,)).fetchone()
        return (int(row[0], 16), row[1]) if row else None

    def _sign(self, db, device: str, entries: list[tuple[int, int]]):
        """(command_id, frame hex, probe) against the known state, or a probe."""
        cred = self._cred(device)
        if cred is None:
            return None
        token = self._token(db, device)
        cid = secrets.randbits(32)
        inc, rev = token if token else (0, 0)
        frame = lc.build_command(oc.ble_write_key(cred), cred.device_id, cred.key_epoch, cid, inc, rev, entries)
        return cid, frame.hex().upper(), token is None

    # -- from the app's operations ------------------------------------------
    def enqueue(self, op: str, device: str, payload: dict) -> bool:
        """A settings operation for a tracker: sign and queue (replaces an older one)."""
        values = payload.get("set") if isinstance(payload, dict) else None
        if not isinstance(values, dict):
            return False
        reported = (self.settings.all().get(device) or {}).get("reported")
        entries = entries_for(values, reported)
        if not entries:
            return False
        now = int(self.clock())
        with closing(sqlite3.connect(self.db_path, timeout=2.0)) as db, db:
            signed = self._sign(db, device, entries)
            if signed is None:
                return False  # no key for it (not a tracker of this farm)
            cid, frame, probe = signed
            db.execute("INSERT INTO lora_queue VALUES (?,?,?,?,?,?,?,0,?,?,NULL) ON CONFLICT(device) DO UPDATE SET "
                       "op=excluded.op, entries=excluded.entries, command_id=excluded.command_id, "
                       "frame=excluded.frame, probe=excluded.probe, state='queued', sent=0, "
                       "updated_utc=excluded.updated_utc, note=NULL",
                       (device, op, json.dumps(entries), cid, frame, int(probe), "queued", now, now))
        return True

    def cancel(self, op: str, device: str) -> None:
        """The operation was settled another way (USB/Bluetooth) or replaced."""
        with closing(sqlite3.connect(self.db_path, timeout=2.0)) as db, db:
            db.execute("DELETE FROM lora_queue WHERE device=? AND op=?", (device, op))

    # -- for the bridges -----------------------------------------------------
    def commands(self) -> list[dict]:
        with closing(sqlite3.connect(self.db_path, timeout=2.0)) as db:
            rows = db.execute("SELECT device, command_id, frame, op, state, probe FROM lora_queue "
                              "ORDER BY created_utc LIMIT 64").fetchall()
        return [{"device": r[0], "command_id": r[1], "hex": r[2], "op": r[3], "state": r[4],
                 "probe": bool(r[5])} for r in rows]

    def events(self, body: object) -> dict:
        if not isinstance(body, dict) or not isinstance(body.get("events"), list) \
                or not 1 <= len(body["events"]) <= MAX_EVENTS:
            raise BadEvent("events")
        done = [self._event(e) for e in body["events"]]
        return {"ok": True, "results": done}

    def _event(self, e: object) -> str:
        if not isinstance(e, dict) or e.get("kind") not in KINDS:
            raise BadEvent("kind")
        if e["kind"] == "result":
            return self._result(e.get("hex"))
        device = e.get("device")
        cid = e.get("command_id")
        if not isinstance(device, str) or not ID16.fullmatch(device.upper()) or \
                not isinstance(cid, int) or isinstance(cid, bool) or not 0 <= cid < 2 ** 32:
            raise BadEvent("command")
        device = device.upper()
        now = int(self.clock())
        with closing(sqlite3.connect(self.db_path, timeout=2.0)) as db, db:
            row = db.execute("SELECT op, command_id, probe, state FROM lora_queue WHERE device=?",
                             (device,)).fetchone()
            if row is None or row[1] != cid:
                return "unknown"
            op, _, probe, state = row
            if e["kind"] == "queued":
                db.execute("UPDATE lora_queue SET state='gateway', updated_utc=? WHERE device=?", (now, device))
            elif e["kind"] == "sent":
                db.execute("UPDATE lora_queue SET sent=sent+1, updated_utc=? WHERE device=?", (now, device))
            else:
                db.execute("UPDATE lora_queue SET note='gateway gave up; handed again', updated_utc=? "
                           "WHERE device=?", (now, device))
        if e["kind"] == "queued":
            try:
                self.ops.result({"op": op, "device": device, "state": "gateway", "via": "lora"})
            except ValueError:
                pass
        return e["kind"]

    def _result(self, text: object) -> str:
        if not isinstance(text, str) or len(text) != 2 * lc.RESULT_SIZE or not HEX.fullmatch(text.upper()):
            raise BadEvent("hex")
        data = bytes.fromhex(text)
        device = data[4:12].hex().upper()
        cred = self._cred(device)
        if cred is None:
            return "unknown"
        try:
            r = lc.parse_result(oc.ble_write_key(cred), data)
        except lc.BadResult:
            return "bad_signature"  # not the tracker's word: changes nothing
        now = int(self.clock())
        with closing(sqlite3.connect(self.db_path, timeout=2.0)) as db, db:
            if r.incarnation or r.revision:
                db.execute("INSERT INTO lora_token VALUES (?,?,?,?) ON CONFLICT(device) DO UPDATE SET "
                           "incarnation=excluded.incarnation, revision=excluded.revision, "
                           "updated_utc=excluded.updated_utc",
                           (device, f"{r.incarnation:016X}", r.revision, now))
            row = db.execute("SELECT op, command_id, entries FROM lora_queue WHERE device=?",
                             (device,)).fetchone()
            if row is None or row[1] != r.command_id:
                return "old"  # an earlier command's answer: only the state was useful
            op, _, entries = row
            if r.outcome in ("STALE", "MAINTENANCE", "UNAVAILABLE", "OUTCOME_UNKNOWN"):
                # Sign again against what the tracker has now (a probe if unknown).
                signed = self._sign(db, device, [tuple(x) for x in json.loads(entries)])
                if signed is not None:
                    cid, frame, probe = signed
                    db.execute("UPDATE lora_queue SET command_id=?, frame=?, probe=?, state='queued', "
                               "updated_utc=?, note=? WHERE device=?",
                               (cid, frame, int(probe), now, r.outcome, device))
                return "resigned"
            db.execute("DELETE FROM lora_queue WHERE device=?", (device,))
        if r.outcome in ("APPLIED", "UNCHANGED"):
            self.ops.result({"op": op, "device": device, "via": "lora",
                             "state": "applied" if r.outcome == "APPLIED" else "same"})
            self.settings.report({"device": device, "values": values_from(r), "via": "lora"})
            return "applied"
        reason = "tasma bu değeri kabul etmedi" if r.outcome == "INVALID" else "izin yok"
        self.ops.result({"op": op, "device": device, "via": "lora", "state": "rejected",
                         "note": f"Tasma reddetti: {reason}."})
        return "rejected"
