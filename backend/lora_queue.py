"""6c: LoRa'dan ayar komutları, sunucu tarafı (sahip kararı 2026-11-11).

Uygulamada bir ayar ya da alan işlemi (device_ops, kind=settings|fence)
oluşunca sunucu o tasma için imzalı bir LoRa komutu hazırlar (tasmanın yazma
anahtarıyla; alan listesi 1-3 çerçeve) ve kuyruğa koyar. Köprüler (PC Edge, telefon)
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
import math
from pathlib import Path
import re
import secrets
import sqlite3
import time
from typing import Callable, Optional

from backend import lora_command as lc
from backend import orun_credentials as oc

# One waiting command per tracker and resource (1 settings, 2 areas).
SCHEMA_QUEUE = ("CREATE TABLE IF NOT EXISTS lora_queue ("
                " device TEXT NOT NULL, resource INTEGER NOT NULL, op TEXT NOT NULL,"
                " content TEXT NOT NULL, command_id INTEGER NOT NULL, frames TEXT NOT NULL,"
                " probe INTEGER NOT NULL, state TEXT NOT NULL, sent INTEGER NOT NULL DEFAULT 0,"
                " created_utc INTEGER NOT NULL, updated_utc INTEGER NOT NULL, note TEXT,"
                " PRIMARY KEY (device, resource))")
SCHEMA_TOKEN = ("CREATE TABLE IF NOT EXISTS lora_token ("
                " device TEXT NOT NULL, resource INTEGER NOT NULL, incarnation TEXT NOT NULL,"
                " revision INTEGER NOT NULL, updated_utc INTEGER NOT NULL, PRIMARY KEY (device, resource))")
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


def _e7(x: float) -> int:
    # The app's Math.round (half up), so both sides send the same vertices.
    return math.floor(x * 1e7 + 0.5)


def polys_for(payload: dict) -> list[list[tuple[int, int]]]:
    """Alan işleminin halkaları (lon, lat) -> tasmanın köşeleri (lat_e7, lon_e7)."""
    zones = payload.get("zones") if isinstance(payload, dict) else None
    if not isinstance(zones, list):
        raise ValueError("zones")
    polys = []
    for z in zones:
        ring = z.get("ring") if isinstance(z, dict) else None
        if not isinstance(ring, list):
            raise ValueError("ring")
        v = [(_e7(float(p[1])), _e7(float(p[0]))) for p in ring]
        if len(v) > 1 and v[0] == v[-1]:
            v.pop()  # closing duplicate, as the app drops it
        polys.append(v)
    return polys


def values_from(r: lc.Result) -> dict:
    services = r.requested_services if r.service_mode == MODE_EXPLICIT else SERVICE_TRACKING
    return {"interval_s": r.interval_s, "ble_window_s": r.ble_window_s,
            "tracking": bool(services & SERVICE_TRACKING), "relay": bool(services & SERVICE_RELAY)}


class LoraQueue:
    def __init__(self, db_path: str | Path, registry: Callable[[], Optional[oc.CredentialRegistry]],
                 ops, settings, fences=None, clock: Callable[[], float] = time.time):
        self.db_path = str(db_path)
        self.registry, self.ops, self.settings, self.fences, self.clock = registry, ops, settings, fences, clock
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

    def _token(self, db, device: str, resource: int) -> Optional[tuple[int, int]]:
        row = db.execute("SELECT incarnation, revision FROM lora_token WHERE device=? AND resource=?",
                         (device, resource)).fetchone()
        return (int(row[0], 16), row[1]) if row else None

    def _sign(self, db, device: str, resource: int, content):
        """(command_id, [frame hex], probe) against the known state, or a probe."""
        cred = self._cred(device)
        if cred is None:
            return None
        token = self._token(db, device, resource)
        cid = secrets.randbits(32)
        inc, rev = token if token else (0, 0)
        key = oc.ble_write_key(cred)
        if resource == lc.RESOURCE_SETTINGS:
            frames = [lc.build_command(key, cred.device_id, cred.key_epoch, cid, inc, rev,
                                       [tuple(e) for e in content])]
        else:
            frames = lc.build_area_command(key, cred.device_id, cred.key_epoch, cid, inc, rev,
                                           [[tuple(v) for v in p] for p in content])
        return cid, [f.hex().upper() for f in frames], token is None

    def _put(self, op: str, device: str, resource: int, content) -> bool:
        now = int(self.clock())
        with closing(sqlite3.connect(self.db_path, timeout=2.0)) as db, db:
            signed = self._sign(db, device, resource, content)
            if signed is None:
                return False  # no key for it (not a tracker of this farm)
            cid, frames, probe = signed
            db.execute("INSERT INTO lora_queue VALUES (?,?,?,?,?,?,?,?,0,?,?,NULL) "
                       "ON CONFLICT(device, resource) DO UPDATE SET op=excluded.op, content=excluded.content, "
                       "command_id=excluded.command_id, frames=excluded.frames, probe=excluded.probe, "
                       "state='queued', sent=0, updated_utc=excluded.updated_utc, note=NULL",
                       (device, resource, op, json.dumps(content), cid, json.dumps(frames), int(probe),
                        "queued", now, now))
        return True

    # -- from the app's operations ------------------------------------------
    def enqueue(self, op: str, device: str, payload: dict, kind: str = "settings") -> bool:
        """A settings or area operation for a tracker: sign and queue (replaces an older one)."""
        if kind == "fence":
            try:
                polys = polys_for(payload)
                lc.area_set(polys)
            except (ValueError, TypeError, IndexError):
                return False
            return self._put(op, device, lc.RESOURCE_AREAS, polys)
        values = payload.get("set") if isinstance(payload, dict) else None
        if not isinstance(values, dict):
            return False
        reported = (self.settings.all().get(device) or {}).get("reported")
        entries = entries_for(values, reported)
        if not entries:
            return False
        return self._put(op, device, lc.RESOURCE_SETTINGS, entries)

    def cancel(self, op: str, device: str) -> None:
        """The operation was settled another way (USB/Bluetooth) or replaced."""
        with closing(sqlite3.connect(self.db_path, timeout=2.0)) as db, db:
            db.execute("DELETE FROM lora_queue WHERE device=? AND op=?", (device, op))

    # -- for the bridges -----------------------------------------------------
    def commands(self) -> list[dict]:
        with closing(sqlite3.connect(self.db_path, timeout=2.0)) as db:
            rows = db.execute("SELECT device, resource, command_id, frames, op, state, probe FROM lora_queue "
                              "ORDER BY created_utc LIMIT 64").fetchall()
        return [{"device": r[0], "resource": r[1], "command_id": r[2], "frames": json.loads(r[3]),
                 "op": r[4], "state": r[5], "probe": bool(r[6])} for r in rows]

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
            row = db.execute("SELECT op, resource FROM lora_queue WHERE device=? AND command_id=?",
                             (device, cid)).fetchone()
            if row is None:
                return "unknown"
            op, resource = row
            if e["kind"] == "queued":
                db.execute("UPDATE lora_queue SET state='gateway', updated_utc=? WHERE device=? AND resource=?",
                           (now, device, resource))
            elif e["kind"] == "sent":
                db.execute("UPDATE lora_queue SET sent=sent+1, updated_utc=? WHERE device=? AND resource=?",
                           (now, device, resource))
            else:
                db.execute("UPDATE lora_queue SET note='gateway gave up; handed again', updated_utc=? "
                           "WHERE device=? AND resource=?", (now, device, resource))
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
                db.execute("INSERT INTO lora_token VALUES (?,?,?,?,?) ON CONFLICT(device, resource) DO UPDATE SET "
                           "incarnation=excluded.incarnation, revision=excluded.revision, "
                           "updated_utc=excluded.updated_utc",
                           (device, r.resource, f"{r.incarnation:016X}", r.revision, now))
            row = db.execute("SELECT op, command_id, content FROM lora_queue WHERE device=? AND resource=?",
                             (device, r.resource)).fetchone()
            if row is None or row[1] != r.command_id:
                return "old"  # an earlier command's answer: only the state was useful
            op, _, content = row
            if r.outcome in ("STALE", "MAINTENANCE", "UNAVAILABLE", "OUTCOME_UNKNOWN"):
                # Sign again against what the tracker has now (a probe if unknown).
                signed = self._sign(db, device, r.resource, json.loads(content))
                if signed is not None:
                    cid, frames, probe = signed
                    db.execute("UPDATE lora_queue SET command_id=?, frames=?, probe=?, state='queued', "
                               "updated_utc=?, note=? WHERE device=? AND resource=?",
                               (cid, json.dumps(frames), int(probe), now, r.outcome, device, r.resource))
                return "resigned"
            db.execute("DELETE FROM lora_queue WHERE device=? AND resource=?", (device, r.resource))
        if r.outcome in ("APPLIED", "UNCHANGED"):
            self.ops.result({"op": op, "device": device, "via": "lora",
                             "state": "applied" if r.outcome == "APPLIED" else "same"})
            if r.resource == lc.RESOURCE_SETTINGS:
                self.settings.report({"device": device, "values": values_from(r), "via": "lora"})
            else:
                self._record_areas(op, device, r)
            return "applied"
        reason = "tasma bu değeri kabul etmedi" if r.outcome == "INVALID" else "izin yok"
        self.ops.result({"op": op, "device": device, "via": "lora", "state": "rejected",
                         "note": f"Tasma reddetti: {reason}."})
        return "rejected"

    def _record_areas(self, op: str, device: str, r: lc.Result) -> None:
        """The tracker holds the operation's areas now: record them like a USB/BLE write."""
        if self.fences is None:
            return
        row = next((o for o in self.ops.recent() if o["op"] == op and o["device"] == device), None)
        payload = row["payload"] if row else {}
        zones = [{"id": z.get("id"), "name": z.get("name")} for z in payload.get("zones", [])
                 if isinstance(z, dict)]
        try:
            self.fences.record({"device": device, "digest": payload.get("digest"), "areas": r.areas,
                                "vertices": r.vertices, "zones": zones})
        except ValueError:
            pass
