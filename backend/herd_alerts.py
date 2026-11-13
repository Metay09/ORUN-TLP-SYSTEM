"""Sürü uyarıları sunucuda (sahip kararı 2026-11-14).

Telefondaki uygulama arka planda güvenilir çalışmıyor (Android, ekrandan
çıkan uygulamanın kodunu dakikalar içinde durduruyor). Bu yüzden "hangi
hayvan için bildirim verilmeli" kararı burada verilir; telefondaki küçük
servis yalnız sorar ve bildirimi gösterir.

Dört uyarı (uygulamadaki src/app/alerts.ts ile aynı kurallar):
  out      hayvan çizilen alanların dışında (son 3 kabul edilen konumun 2'si
           ve en yenisi dışarıda); serbest otlatmada yok
  back     dışarıdaki hayvan alana döndü (son 12 saatte)
  silent / lost   haber alınamıyor: 3 rapor kaçtı (en az 30 dk, en çok 24 sa);
           iki katında ya da alan dışındayken ilk eşikte "kayıp olabilir"
  battery  tasma pili bitiyor

Bu modül yalnız OKUR: doğrulanmış kayıt defteri (düz içerik sunucuda zaten
durur), cihaz adları, tasmanın onayladığı rapor aralığı ve hesaba kaydedilen
alanlar. Her uyarının kalıcı bir anahtarı vardır; telefon her anahtarı bir kez
gösterir.

Alanlar (HerdZones): uygulamada çizilen alan kümesi artık hesaba da yazılır
(tek belge). Tasmaya gönderilen alan ayrı bir kayıttır (tracker_fences.py).
"""
from __future__ import annotations

from contextlib import closing
import json
import math
from pathlib import Path
import sqlite3
import time
from typing import Callable

MIN_MS = 60_000
HOUR_MS = 60 * MIN_MS
DAY_MS = 24 * HOUR_MS
WINDOW_MS = 14 * DAY_MS          # records older than this are not looked at
BACK_MS = 12 * HOUR_MS           # "alana döndü" is news for this long
MAX_ROWS = 200_000               # newest rows read from the ledger per call

ZONES_SCHEMA = ("CREATE TABLE IF NOT EXISTS herd_zones ("
                " id INTEGER PRIMARY KEY CHECK(id=1), doc TEXT NOT NULL, updated_utc INTEGER NOT NULL)")
MAX_ZONES, MAX_VERTICES, MAX_NAME, MAX_ID = 100, 64, 40, 64

# Same table as firmware battery_policy.cpp and the app (herd.ts CURVE).
CURVE = ((3400, 0), (3440, 10), (3480, 20), (3530, 30), (3630, 40), (3720, 50),
         (3800, 60), (3890, 70), (3990, 80), (4050, 90), (4190, 100))
POWER = ("UNKNOWN", "BATTERY", "CHARGING", "FULL", "NO_BATTERY")


class BadZones(ValueError):
    pass


# ------------------------------------------------------------------ zones ---
def _num(v: object) -> bool:
    return isinstance(v, (int, float)) and not isinstance(v, bool) and math.isfinite(v)


def clean_zones(body: object) -> dict:
    """The app's zone book, reduced to what the alert rule needs."""
    if not isinstance(body, dict) or not isinstance(body.get("zones"), list):
        raise BadZones("zones")
    if len(body["zones"]) > MAX_ZONES:
        raise BadZones("too_many")
    zones, seen = [], set()
    for z in body["zones"]:
        if not isinstance(z, dict):
            raise BadZones("zone")
        zid, name, ring = z.get("id"), z.get("name"), z.get("ring")
        if (not isinstance(zid, str) or not 0 < len(zid) <= MAX_ID or zid in seen or
                not isinstance(name, str) or not name.strip() or
                not isinstance(z.get("active"), bool) or
                not isinstance(ring, list) or not 3 <= len(ring) <= MAX_VERTICES):
            raise BadZones("zone")
        seen.add(zid)
        points = []
        for p in ring:
            if (not isinstance(p, list) or len(p) != 2 or not _num(p[0]) or not _num(p[1]) or
                    not -180 < p[0] < 180 or not -90 < p[1] < 90):
                raise BadZones("ring")
            points.append([float(p[0]), float(p[1])])
        updated = z.get("updated", 0)
        if not _num(updated) or updated < 0:
            raise BadZones("updated")
        zones.append({"id": zid, "name": name.strip()[:MAX_NAME], "active": z["active"],
                      "ring": points, "updated": int(updated)})
    graze = body.get("graze") or {}
    until, forever = graze.get("until") if isinstance(graze, dict) else None, False
    if isinstance(graze, dict):
        forever = graze.get("forever") is True
    if until is not None and (not _num(until) or until < 0):
        raise BadZones("graze")
    changed = body.get("changed", 0)
    if not _num(changed) or changed < 0:
        raise BadZones("changed")
    return {"zones": zones, "graze": {"until": int(until) if until is not None else None, "forever": forever},
            "changed": int(changed)}


class HerdZones:
    def __init__(self, db_path: str | Path, clock: Callable[[], float] = time.time):
        self.db_path = str(db_path)
        self.clock = clock
        with closing(sqlite3.connect(self.db_path, timeout=2.0)) as db, db:
            db.execute(ZONES_SCHEMA)

    def get(self) -> dict:
        with closing(sqlite3.connect(self.db_path, timeout=2.0)) as db:
            row = db.execute("SELECT doc FROM herd_zones WHERE id=1").fetchone()
        return json.loads(row[0]) if row else {"zones": [], "graze": {"until": None, "forever": False}, "changed": 0}

    def set(self, body: object) -> dict:
        doc = clean_zones(body)
        with closing(sqlite3.connect(self.db_path, timeout=2.0)) as db, db:
            db.execute("INSERT INTO herd_zones VALUES (1,?,?) ON CONFLICT(id) DO UPDATE SET "
                       "doc=excluded.doc, updated_utc=excluded.updated_utc",
                       (json.dumps(doc, sort_keys=True), int(self.clock())))
        return {"zones": len(doc["zones"]), "active": sum(1 for z in doc["zones"] if z["active"])}


# --------------------------------------------------------------- geometry ---
# Exact integer port of the app's zones.ts classifyE7 (itself the firmware's
# geofence_geometry.cpp): [latitude_e7, longitude_e7], boundary counts inside.
def _e7(v: float) -> int:
    return math.floor(v * 1e7 + 0.5)


def _cross(a, b, p) -> int:
    return (b[1] - a[1]) * (p[0] - a[0]) - (b[0] - a[0]) * (p[1] - a[1])


def _on_segment(a, b, p) -> bool:
    return (_cross(a, b, p) == 0 and min(a[0], b[0]) <= p[0] <= max(a[0], b[0]) and
            min(a[1], b[1]) <= p[1] <= max(a[1], b[1]))


def inside(v: list[tuple[int, int]], p: tuple[int, int]) -> bool:
    lats, lons = [c[0] for c in v], [c[1] for c in v]
    if p[0] < min(lats) or p[0] > max(lats) or p[1] < min(lons) or p[1] > max(lons):
        return False
    n = len(v)
    if any(_on_segment(v[i], v[(i + 1) % n], p) for i in range(n)):
        return True
    w = 0
    for i in range(n):
        a, b = v[i], v[(i + 1) % n]
        if a[0] <= p[0]:
            if b[0] > p[0] and _cross(a, b, p) > 0:
                w += 1
        elif b[0] <= p[0] and _cross(a, b, p) < 0:
            w -= 1
    return w != 0


def compile_fence(doc: dict) -> tuple[list[list[tuple[int, int]]], int]:
    """Active polygons in E7 and the moment the checked set last changed (ms)."""
    polys, since = [], int(doc.get("changed") or 0)
    for z in doc.get("zones", []):
        if not z.get("active"):
            continue
        v = [(_e7(lat), _e7(lon)) for lon, lat in z["ring"]]
        if len(v) > 1 and v[0] == v[-1]:
            v.pop()
        if len(v) >= 3:
            polys.append(v)
            since = max(since, int(z.get("updated") or 0))
    return polys, since


# ---------------------------------------------------------------- records ---
def _u(b: bytes) -> int:
    return int.from_bytes(b, "big")


def _i(b: bytes) -> int:
    return int.from_bytes(b, "big", signed=True)


def decode(p: bytes) -> dict | None:
    """What the rules need from a PERIODIC plaintext (= tools/orun_edge_bench.describe_periodic)."""
    if len(p) != 64 or not p[1] & 1:
        return None                      # no trusted time: cannot be ordered
    out = {"t": _u(p[16:20]) * 1000, "period_s": _u(p[20:24]), "lat": None, "lon": None, "hdop": None,
           "mv": None, "word": None, "power": None, "usb": None}
    if p[1] & 2:
        out["lat"], out["lon"] = _i(p[24:28]), _i(p[28:32])      # already E7
        if p[2] == 1:
            out["hdop"] = _u(p[36:38]) / 100
    if p[1] & 32:
        out["mv"] = _u(p[56:58])
        out["word"] = p[3]                                        # 2 LOW, 3 CRITICAL
        power = (p[42] >> 2) & 0x07
        out["power"] = POWER[power] if 0 < power < len(POWER) else None
        out["usb"] = bool(p[42] & 0x20) if power else None
    return out


def battery_percent(mv: int) -> int:
    if mv <= CURVE[0][0]:
        return 0
    for (lm, lp), (hm, hp) in zip(CURVE, CURVE[1:]):
        if mv <= hm:
            return lp + ((mv - lm) * (hp - lp) + (hm - lm) // 2) // (hm - lm)
    return 100


def battery_level(r: dict) -> str:
    """good / low / critical / unknown (= the app's herd.ts batteryOf)."""
    if r["mv"] is None or r["power"] == "NO_BATTERY":
        return "unknown"
    if r["power"] in ("FULL", "CHARGING"):
        return "good"
    pct = battery_percent(r["mv"])
    if r["word"] == 3 or pct <= 5:
        return "critical"
    if r["word"] == 2 or pct <= 20:
        return "low"
    return "good"


# ------------------------------------------------------------------ words ---
def _since(ms: int, now: int) -> str:
    d = max(0, now - ms)
    if d < HOUR_MS:
        return f"{max(1, d // MIN_MS)} dakikadır"
    if d < 2 * DAY_MS:
        return f"{d // HOUR_MS} saattir"
    return f"{d // DAY_MS} gündür"


def _duration(ms: int) -> str:
    m = round(max(0, ms) / MIN_MS)
    if m < 60:
        return f"{m} dk"
    if m < 48 * 60:
        return f"{m // 60} sa {m % 60} dk" if m % 60 else f"{m // 60} sa"
    h = round(m / 60)
    return f"{h // 24} gün {h % 24} sa" if h % 24 else f"{h // 24} gün"


def aging_limit(period_ms: int) -> int:
    return min(max(3 * period_ms, 30 * MIN_MS), DAY_MS)


# ------------------------------------------------------------------ rules ---
def outside_episodes(points: list[dict], polys, since: int) -> list[dict]:
    """Confirmed outside periods (oldest first): {"from", "to", "key"}; = zones.ts outsideEpisodes."""
    if not polys:
        return []
    out, win, state, cur = [], [], None, None
    for r in points:
        if r["lat"] is None or (r["hdop"] is not None and r["hdop"] > 5):
            continue
        win.append((not any(inside(v, (r["lat"], r["lon"])) for v in polys), r["t"]))
        win = win[-3:]
        outs = sum(1 for o, _ in win if o)
        newest_out = win[-1][0]
        if newest_out and outs >= 2 and state != "out":
            state = "out"
            cur = {"from": next(t for o, t in win if o), "to": None}
            out.append(cur)
        elif not newest_out and len(win) - outs >= 2 and state != "in":
            if cur:
                cur["to"] = next(t for o, t in win if not o)
            cur, state = None, "in"
    # Nothing is said about the time before the zone set last changed.
    kept = []
    for e in out:
        if since and e["to"] is not None and e["to"] <= since:
            continue
        if since and e["from"] < since:
            e = {**e, "from": since}
        kept.append(e)
    return kept


def device_alerts(dev: str, name: str, rows: list[dict], polys, since: int, grazing: bool,
                  interval: tuple[int, int] | None, now: int) -> list[dict]:
    """`rows`: this tracker's decoded records, oldest first. `interval`: (seconds, confirmed at ms)."""
    alerts: list[dict] = []
    located = [r for r in rows if r["lat"] is not None]
    if not rows:
        return alerts

    def add(kind: str, key: str, title: str, body: str, **extra) -> None:
        alerts.append({"key": key, "kind": kind, "device": dev, "title": title, "body": body, **extra})

    episodes = outside_episodes(located, polys, since)
    open_out = next((e for e in episodes if e["to"] is None), None)
    for e in episodes:
        key = f"out:{dev}:{e['from']}"
        if e["to"] is None:
            if not grazing:
                add("out", key, f"{name} alan dışına çıktı",
                    f"{_since(e['from'], now).capitalize()} açık alanların dışında")
        elif now - e["to"] <= BACK_MS:
            add("back", "back:" + key, f"{name} alana döndü",
                f"{_duration(e['to'] - e['from'])} dışarıda kaldı", needs=key)

    # Not heard from. The collar reports 3x as often outside its own zones and
    # falls back to the base interval afterwards, so a "20 dk" record may be
    # followed by one a full base interval later: wait 1.25 x base (3 x 0.42).
    heard = rows[-1]["t"]
    last_period = (located[-1] if located else rows[-1])["period_s"] or 60
    period_s = max(30, last_period)
    if interval:
        period_s = max(period_s, interval[0] if interval[1] > heard else math.ceil(interval[0] * 0.42))
    T = period_s * 1000
    quiet = now - heard
    if quiet > aging_limit(T):
        lost = quiet > 2 * aging_limit(T) or (open_out is not None and not grazing)
        if lost:
            add("lost", f"lost:{dev}:{heard}", f"{name} kayıp olabilir",
                f"{_since(heard, now).capitalize()} hiç haber yok · son konumuna bakın")
        else:
            add("silent", f"silent:{dev}:{heard}", f"{name} sessiz",
                f"{_since(heard, now).capitalize()} konum gelmiyor · {max(1, round(quiet / T) - 1)} rapor kaçtı")

    # Battery: an episode opens at low/critical and closes after a day back at good.
    start, critical, last_bad = None, False, 0
    for r in rows:
        level = battery_level(r)
        if level == "unknown":
            continue
        if level in ("low", "critical"):
            last_bad = r["t"]
            if start is None:
                start, critical = r["t"], False
            critical = critical or level == "critical"
        elif start is not None and r["t"] - last_bad >= DAY_MS:
            start, critical = None, False
    if start is not None and critical:
        add("battery", f"battery:{dev}:{start // DAY_MS}", f"{name}: pil bitiyor",
            "Tasma yakında susacak · bugün şarj edin")
    return alerts


class HerdAlerts:
    def __init__(self, db_path: str | Path, zones: HerdZones, clock: Callable[[], float] = time.time):
        self.db_path = str(db_path)
        self.zones = zones
        self.clock = clock

    def _rows(self, now: int) -> dict[str, list[dict]]:
        path = Path(self.db_path)
        by: dict[str, list[dict]] = {}
        with closing(sqlite3.connect(path.resolve().as_uri() + "?mode=ro", uri=True, timeout=2.0)) as db:
            for dev, plain in db.execute(
                    "SELECT device_id, plaintext FROM verified_observations WHERE family=1 "
                    "ORDER BY rowid DESC LIMIT ?", (MAX_ROWS,)):
                r = decode(plain) if isinstance(plain, bytes) and isinstance(dev, bytes) else None
                if r is None or not now - WINDOW_MS <= r["t"] <= now + 10 * MIN_MS:
                    continue
                by.setdefault(dev.hex().upper(), []).append(r)
        for rows in by.values():
            rows.sort(key=lambda r: r["t"])
        return by

    def current(self, names: dict[str, str], settings: dict[str, dict]) -> dict:
        now = int(self.clock() * 1000)
        doc = self.zones.get()
        polys, since = compile_fence(doc)
        graze = doc.get("graze") or {}
        grazing = bool(graze.get("forever")) or (graze.get("until") is not None and graze["until"] > now)
        alerts: list[dict] = []
        for dev, rows in sorted(self._rows(now).items()):
            rep = (settings.get(dev) or {}).get("reported") or {}
            at = (settings.get(dev) or {}).get("reported_utc")
            s = rep.get("interval_s")
            interval = (int(s), int(at) * 1000) if isinstance(s, int) and s > 0 and isinstance(at, int) else None
            alerts += device_alerts(dev, names.get(dev) or f"ORUN-{dev[-8:]}", rows, polys, since,
                                    grazing, interval, now)
        return {"now": now, "alerts": alerts}
