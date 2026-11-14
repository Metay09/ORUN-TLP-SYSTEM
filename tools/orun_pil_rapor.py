#!/usr/bin/env python3
"""Pil denemesi raporu (Y3, #307): sunucu kayıt defterinden, yalnız okuyarak.

Her tasma için pille geçen, rapor aralığı sabit dönemleri (2 / 5 / 60 dk ...)
ayrı ayrı çıkarır: süre, mV, harcanan mAh, ortalama akım, gelen/beklenen rapor.
Sonra uygulamanın pil ömrü modelini (edge/app/src/app/batteryLife.ts, aynı
kurallar) bu dönemlerle kurar:

    ortalama akım = bekleme akımı + rapor başına maliyet x saatteki rapor

ve uygulamanın bugünkü varsayılanıyla karşılaştırır. Hiçbir şey yazmaz.

  python3 tools/orun_pil_rapor.py                 # çalışan orun-sunucu'nun defteri
  python3 tools/orun_pil_rapor.py --db ~/orun-backend-bench.sqlite
  python3 tools/orun_pil_rapor.py --gun 3 --mah 450 --tasma 52E1
  python3 tools/orun_pil_rapor.py --csv ~/pil.csv  # ham okumalar da
"""
from __future__ import annotations

import argparse
import csv
import datetime as dt
from pathlib import Path
import sqlite3
import statistics
import sys

HOUR = 3600.0

# --- uygulamayla aynı sayılar (batteryLife.ts, herd.ts) ---------------------
DEFAULT_BASE_MA, DEFAULT_PACKET_MAH = 0.15, 0.15
BASE_MIN_MA, PACKET_MIN_MAH = 0.02, 0.02
RUN_MIN_H, RUN_MIN_POINTS, SETTLE_S = 6, 8, HOUR
USABLE = 0.8
GNSS_ON_MA, SHORT_INTERVAL_S = 25, 60
CURVE = [(3400, 0), (3440, 10), (3480, 20), (3530, 30), (3630, 40), (3720, 50),
         (3800, 60), (3890, 70), (3990, 80), (4050, 90), (4190, 100)]
POWER = ("UNKNOWN", "BATTERY", "CHARGING", "FULL", "NO_BATTERY")


def pct(mv: float) -> float:
    if mv <= CURVE[0][0]:
        return 0.0
    for (lm, lp), (hm, hp) in zip(CURVE, CURVE[1:]):
        if mv <= hm:
            return lp + (mv - lm) * (hp - lp) / (hm - lm)
    return 100.0


def u(b: bytes) -> int:
    return int.from_bytes(b, "big")


# --- kayıtlar ---------------------------------------------------------------
def server_db() -> Path:
    """--db of the running orun-sunucu (its systemd ExecStart), else the usual file."""
    import re
    import shlex
    import subprocess
    try:
        show = subprocess.run(["systemctl", "--user", "show", "-p", "ExecStart", "--value",
                               "orun-sunucu.service"], capture_output=True, text=True).stdout
        m = re.search(r"argv\[\]=([^;]*)", show)
        argv = shlex.split(m.group(1)) if m else []
        for i, x in enumerate(argv):
            if x == "--db" and i + 1 < len(argv):
                return Path(argv[i + 1]).expanduser()
            if x.startswith("--db="):
                return Path(x[5:]).expanduser()
    except Exception:  # noqa: BLE001 — yalnız yolu bulmak için
        pass
    guess = Path.home() / "orun-backend-bench.sqlite"
    if guess.is_file():
        return guess
    raise SystemExit("orun-sunucu'nun defteri bulunamadı; --db <yol> ile verin")


def read(db_path: Path, since: float | None):
    if not db_path.is_file():
        raise SystemExit(f"defter yok: {db_path}")
    db = sqlite3.connect(db_path.resolve().as_uri() + "?mode=ro", uri=True, timeout=5)
    tables = {r[0] for r in db.execute("SELECT name FROM sqlite_master WHERE type='table'")}
    mah = dict(db.execute("SELECT device,mah FROM device_battery")) if "device_battery" in tables else {}
    names = dict(db.execute("SELECT device,name FROM device_names")) if "device_names" in tables else {}
    join = "LEFT JOIN edge_receipts r USING (device_id,incarnation,sequence)" if "edge_receipts" in tables else ""
    seen = "r.first_seen_utc" if join else "NULL"
    rows = db.execute(f"SELECT o.device_id,o.plaintext,{seen} FROM verified_observations o {join} "
                      "WHERE o.family=1").fetchall()
    db.close()
    out: dict[str, list[dict]] = {}
    for dev, p, first_seen in rows:
        if len(p) != 64:
            continue
        flags = p[1]
        t = u(p[16:20]) if flags & 1 else first_seen
        if not t:
            continue
        rec = {"t": float(t), "inc": p[4:12].hex().upper(), "seq": u(p[12:16]),
               "period": u(p[20:24]), "mv": None, "power": None, "usb": None,
               "fix": bool(flags & 2), "gnss_fail": p[60] if flags & 64 else None}
        if flags & 32:
            rec["mv"] = u(p[56:58])
            ps = (p[42] >> 2) & 0x07
            rec["power"] = POWER[ps] if 0 < ps < len(POWER) else None
            rec["usb"] = bool(p[42] & 0x20) if ps else None
        if since is None or rec["t"] >= since:
            out.setdefault(dev.hex().upper(), []).append(rec)
    for v in out.values():
        v.sort(key=lambda r: (r["t"], r["seq"]))
    return out, mah, names


# --- dönemler (drainRuns ile aynı kural) ------------------------------------
def runs(series: list[dict], mah: int) -> list[dict]:
    found, cur, off_since = [], [], 0.0

    def close():
        nonlocal cur
        pts, cur = cur, []
        if len(pts) < RUN_MIN_POINTS:
            return
        k = max(1, min(5, len(pts) // 3))
        head, tail = pts[:k], pts[-k:]
        hours = (statistics.median(x["t"] for x in tail) - statistics.median(x["t"] for x in head)) / HOUR
        mv0 = statistics.median(x["mv"] for x in head)
        mv1 = statistics.median(x["mv"] for x in tail)
        used = (statistics.median(pct(x["mv"]) for x in head) -
                statistics.median(pct(x["mv"]) for x in tail)) / 100 * mah
        period = round(statistics.median(x["period"] for x in pts))
        seqs = [x["seq"] for x in pts if x["inc"] == pts[0]["inc"]]
        expected = (pts[-1]["t"] - pts[0]["t"]) / period + 1
        found.append({
            "start": pts[0]["t"], "end": pts[-1]["t"], "hours": hours, "period": period,
            "packets": hours * HOUR / period, "mAh": used, "mv0": mv0, "mv1": mv1,
            "got": len(pts), "expected": expected,
            "seq_gap": (max(seqs) - min(seqs) + 1 - len(set(seqs))) if seqs else 0,
            "resets": len({x["inc"] for x in pts}) - 1,
            "nofix": sum(1 for x in pts if not x["fix"]),
            "used": hours >= RUN_MIN_H and used > 0,
        })

    for r in series:
        if r["mv"] is None:
            continue
        if r["usb"] is True or r["power"] in ("NO_BATTERY", "FULL"):
            close()
            off_since = 0.0
            continue
        if not r["period"]:
            continue
        if not off_since:
            off_since = r["t"]
        # period_s is the measured length of each report period (main.cpp
        # finishProductPeriod), so it wanders by the GPS fix time: 3563, 3601...
        # A run ends only when the interval itself changes (more than 25 %).
        if cur and abs(r["period"] - cur[0]["period"]) > 0.25 * cur[0]["period"]:
            close()
        if r["t"] - off_since < SETTLE_S:
            continue
        cur.append(r)
    close()
    return found


def fit(rs: list[dict]) -> tuple[float, float, str]:
    rs = [r for r in rs if r["used"]]
    if not rs:
        return DEFAULT_BASE_MA, DEFAULT_PACKET_MAH, "none"
    hh = sum(r["hours"] ** 2 for r in rs); kk = sum(r["packets"] ** 2 for r in rs)
    hk = sum(r["hours"] * r["packets"] for r in rs)
    hm = sum(r["hours"] * r["mAh"] for r in rs); km = sum(r["packets"] * r["mAh"] for r in rs)
    H = sum(r["hours"] for r in rs); K = sum(r["packets"] for r in rs); M = sum(r["mAh"] for r in rs)
    rates = [r["packets"] / r["hours"] for r in rs]
    det = hh * kk - hk * hk
    if max(rates) >= 2 * min(rates) and det > 0:
        base, pkt = (hm * kk - km * hk) / det, (km * hh - hm * hk) / det
        if base >= BASE_MIN_MA and pkt >= PACKET_MIN_MAH:
            return base, pkt, "both"
    if DEFAULT_PACKET_MAH * K > DEFAULT_BASE_MA * H * 1.001:
        return DEFAULT_BASE_MA, max(PACKET_MIN_MAH, (M - DEFAULT_BASE_MA * H) / K), "one"
    return max(BASE_MIN_MA, (M - DEFAULT_PACKET_MAH * K) / H), DEFAULT_PACKET_MAH, "one"


def avg_ma(base: float, pkt: float, interval_s: int) -> float:
    return base + pkt * 3600 / interval_s + (GNSS_ON_MA if interval_s < SHORT_INTERVAL_S else 0)


def days(mah: int, ma: float) -> str:
    d = mah * USABLE / (ma * 24)
    return f"{d * 24:.0f} saat" if d < 2 else f"{d:.0f} gün" if d < 60 else f"{d / 30:.1f} ay"


def when(t: float) -> str:
    return dt.datetime.fromtimestamp(t).strftime("%d.%m %H:%M")


def mins(s: int) -> str:
    return f"{round(s / 60)} dk" if s >= 90 else f"{s} sn"


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="ORUN pil denemesi raporu (salt okunur)")
    ap.add_argument("--db", type=Path, help="kayıt defteri (varsayılan: çalışan orun-sunucu'nunki)")
    ap.add_argument("--gun", type=float, default=14, help="son kaç gün (varsayılan 14)")
    ap.add_argument("--mah", type=int, help="pil kapasitesi (hesapta yoksa)")
    ap.add_argument("--tasma", help="yalnız bu tasma (kimliğin başı yeter)")
    ap.add_argument("--csv", type=Path, help="ham okumaları bu dosyaya yaz")
    a = ap.parse_args(argv)

    db = a.db.expanduser() if a.db else server_db()
    since = dt.datetime.now().timestamp() - a.gun * 86400 if a.gun else None
    data, mahs, names = read(db, since)
    print(f"Defter: {db}  ·  son {a.gun:g} gün  ·  {sum(map(len, data.values()))} rapor")
    writer = None
    if a.csv:
        fh = open(a.csv, "w", newline="")
        writer = csv.writer(fh)
        writer.writerow(["tasma", "zaman", "mv", "yuzde", "guc", "usb", "aralik_s", "seq", "konum"])

    for dev, series in sorted(data.items()):
        if a.tasma and not dev.startswith(a.tasma.upper()):
            continue
        name = names.get(dev, "")
        mah = a.mah or mahs.get(dev)
        bat = [r for r in series if r["mv"] is not None]
        print(f"\n=== {name or '(adsız)'}  {dev}  ·  pil {mah or '?'} mAh  ·  {len(series)} rapor, "
              f"{len(bat)} pil okuması")
        if writer:
            for r in series:
                writer.writerow([dev, dt.datetime.fromtimestamp(r["t"]).isoformat(timespec="seconds"),
                                 r["mv"], f"{pct(r['mv']):.1f}" if r["mv"] else "", r["power"], r["usb"],
                                 r["period"], r["seq"], int(r["fix"])])
        if not bat:
            continue
        last = bat[-1]
        hist: dict[int, int] = {}
        for r in bat:
            hist[r["period"]] = hist.get(r["period"], 0) + 1
        top = sorted(hist.items(), key=lambda kv: -kv[1])[:6]
        print("Kayıttaki aralıklar (sn × adet): " + ", ".join(f"{k}×{v}" for k, v in top))
        print(f"Son: {when(last['t'])}  {last['mv']} mV (%{pct(last['mv']):.0f})  {last['power'] or '-'}"
              f"{'  USB' if last['usb'] else ''}  aralık {mins(last['period'])}")
        if not mah:
            print("  pil kapasitesi yok: --mah ile verin")
            continue
        rs = runs(series, mah)
        if not rs:
            print("  Pille geçen, aralığı sabit dönem yok (en az 8 okuma; ilk 1 saat sayılmaz).")
            continue
        print(f"  {'aralık':>6} {'başlangıç':>11} {'bitiş':>11} {'saat':>5} {'mV':>11} "
              f"{'mAh':>5} {'ort.mA':>6} {'gelen/bekl.':>11} {'boşluk':>6} {'reset':>5} {'konumsuz':>8}")
        for r in rs:
            ma = r["mAh"] / r["hours"] if r["hours"] > 0 else 0
            mark = "" if r["used"] else "  (sayılmadı: <6 saat ya da düşüş yok)"
            print(f"  {mins(r['period']):>6} {when(r['start']):>11} {when(r['end']):>11} {r['hours']:5.1f} "
                  f"{r['mv0']:5.0f}→{r['mv1']:<5.0f} {r['mAh']:5.1f} {ma:6.2f} "
                  f"{r['got']:>5}/{r['expected']:<5.0f} {r['seq_gap']:>6} {r['resets']:>5} {r['nofix']:>8}{mark}")
        base, pkt, how = fit(rs)
        word = {"both": "iki aralıktan ölçüldü", "one": "tek aralık: biri varsayılan",
                "none": "ölçüm yok, varsayılan"}[how]
        print(f"  Model ({word}): bekleme {base:.3f} mA  ·  rapor başına {pkt:.3f} mAh "
              f"(uygulama varsayılanı {DEFAULT_BASE_MA} mA · {DEFAULT_PACKET_MAH} mAh)")
        print("  Tahmini ömür (tam pil):  " + "  ·  ".join(
            f"{mins(s)}: {avg_ma(base, pkt, s):.2f} mA ≈ {days(mah, avg_ma(base, pkt, s))}"
            for s in (120, 300, 900, 3600)))
    if writer:
        fh.close()
        print(f"\nHam okumalar: {a.csv}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
