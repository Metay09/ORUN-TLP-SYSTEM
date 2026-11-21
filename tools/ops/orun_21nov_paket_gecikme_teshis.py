#!/usr/bin/env python3
"""21 Kasim 2026 ORUN iki tasma gecikme zinciri. Salt okunur; servis/cihaz degistirmez."""
from __future__ import annotations
from collections import Counter, defaultdict
from datetime import datetime
from pathlib import Path
import re
import shlex
import sqlite3
import subprocess

START = int(datetime(2026, 11, 21).timestamp())
END = int(datetime(2026, 11, 22).timestamp())
EDGE = Path.home() / ".local/share/orun/pc-edge-inbox.sqlite"

def stamp(value):
    return datetime.fromtimestamp(value).strftime("%d.%m %H:%M:%S") if value else "-"

def read_db(path):
    return sqlite3.connect(path.resolve(strict=True).as_uri() + "?mode=ro", uri=True, timeout=8)

def db_argument(argv):
    for i, x in enumerate(argv):
        if x == "--db" and i + 1 < len(argv):
            return Path(argv[i + 1]).expanduser()
        if x.startswith("--db="):
            return Path(x.partition("=")[2]).expanduser()
    return None

def backend_path():
    candidates = []
    for proc in Path("/proc").iterdir():
        if not proc.name.isdigit():
            continue
        try:
            argv = (proc / "cmdline").read_bytes().strip(b"\0").decode(errors="replace").split("\0")
        except (OSError, PermissionError):
            continue
        if any(arg.endswith(("orun_sunucu.py", "orun_backend_bench.py")) for arg in argv):
            p = db_argument(argv)
            if p:
                candidates.append(p)
    for name in ("orun-sunucu.service", "orun-backend.service"):
        try:
            r = subprocess.run(["systemctl", "--user", "show", "-p", "ExecStart", "--value", name],
                               capture_output=True, text=True, timeout=5)
            match = re.search(r"argv\[\]=([^;]*)", r.stdout)
            if match:
                p = db_argument(shlex.split(match.group(1)))
                if p:
                    candidates.append(p)
        except (OSError, ValueError, subprocess.TimeoutExpired):
            pass
    candidates.append(Path.home() / "orun-backend-bench.sqlite")
    for p in candidates:
        try:
            with read_db(p) as db:
                tables = {r[0] for r in db.execute("SELECT name FROM sqlite_master WHERE type='table'")}
                if {"verified_observations", "edge_receipts"}.issubset(tables):
                    return p
        except (OSError, sqlite3.Error):
            pass
    return None

def top_gaps(events, label, maxrows=10):
    events = sorted(set(events), key=lambda x: (x[0], x[1]))
    gaps = sorted(((b[0] - a[0], a, b) for a, b in zip(events, events[1:])
                  if b[0] > a[0]), reverse=True)
    print(" ", label, "| ornek", len(events))
    if not gaps:
        print("    Karsilastirilacak ardil veri yok.")
        return
    vals = sorted(x[0] for x in gaps)
    print("    aralik: ortanca %.1f dk | p95 %.1f dk | en uzun %.1f dk" %
          (vals[len(vals)//2]/60, vals[min(len(vals)-1, int(len(vals)*0.95))]/60,
           vals[-1]/60))
    for duration, a, b in gaps[:maxrows]:
        print("    %s -> %s | %.1f dk | %s -> %s" %
              (stamp(a[0]), stamp(b[0]), duration/60, a[1], b[1]))

def delays(rows, label, maxrows=10):
    rows = sorted((v, desc) for v, desc in rows if v >= 0)
    print(" ", label, "| n", len(rows))
    if not rows:
        return
    vals = [x[0] for x in rows]
    print("    ortanca %.1f dk | p95 %.1f dk | en uzun %.1f dk" %
          (vals[len(vals)//2]/60, vals[min(len(vals)-1, int(len(vals)*.95))]/60, vals[-1]/60))
    for v, desc in rows[-maxrows:][::-1]:
        print("    %.1f dk | %s" % (v/60, desc))

if not EDGE.is_file():
    raise SystemExit("DUR: PC Edge DB yok: " + str(EDGE))
print("=== ORUN 21 KASIM 2026 | BASLANGIC-BITIS GECIKME TESHISI ===")
print("PC Edge:", EDGE)
with read_db(EDGE) as db:
    edge = defaultdict(list)
    for rid, frame, state, first, ack, gw in db.execute("""
        SELECT id,frame,backend_state,first_seen_utc,last_attempt_utc,gateway
        FROM sealed_records
        WHERE first_seen_utc>=? AND first_seen_utc<?
        ORDER BY first_seen_utc,id
    """, (START, END)):
        raw = bytes(frame)
        if len(raw) not in (84, 100):
            continue
        dev = raw[8:16].hex().upper()
        edge[dev].append(dict(id=rid, family=raw[3], mode=raw[4],
                              counter=int.from_bytes(raw[20:28], "big"),
                              first=first, ack=ack, state=state or "PENDING",
                              gateway=gw or "-"))

for dev, rows in sorted(edge.items()):
    print("\n--- TASMA", dev, "| PC Edge gorulen", len(rows), "---")
    print("  Ilk", stamp(rows[0]["first"]), "| Son", stamp(rows[-1]["first"]))
    print("  Modlar:", dict(Counter(x["mode"] for x in rows)))
    print("  Sonuclar:", dict(Counter(x["state"] for x in rows)))
    for mode in (0, 3):
        subset = [x for x in rows if x["family"] == 1 and x["mode"] == mode]
        top_gaps([(x["first"], "ctr=%s/id=%s" % (x["counter"], x["id"]))
                  for x in subset], "Mod %s Edge'e gelis bosluklari" % mode)
        if subset:
            counts = Counter(datetime.fromtimestamp(x["first"]).hour for x in subset)
            print("    saatlik:", " ".join("%02d:%s" % (h,counts[h]) for h in sorted(counts)))
    delays([(x["ack"]-x["first"], "id=%s/ctr=%s %s -> %s (%s)" %
             (x["id"],x["counter"],stamp(x["first"]),stamp(x["ack"]),x["state"]))
            for x in rows if x["ack"] and x["state"] in ("STORED","DUPLICATE")],
           "Edge ilk gorus -> sunucu sonucunu yerelde isaretleme")
    for x in rows:
        if x["state"] in ("REJECTED","CONFLICT"):
            print("  RED/HATA id=%s ctr=%s %s -> %s | %s" %
                  (x["id"],x["counter"],stamp(x["first"]),stamp(x["ack"]),x["state"]))

server = backend_path()
if server is None:
    print("\nUYARI: Sunucu defteri bu bilgisayarda bulunamadi. Edge zamanlari yukarida.")
else:
    print("\n=== SUNUCU / GPS OLCUM ZAMANI KARSILASTIRMASI ===")
    print("Sunucu:", server)
    with read_db(server) as db:
        tables = {r[0] for r in db.execute("SELECT name FROM sqlite_master WHERE type='table'")}
        upload_expr = ("""(SELECT MIN(u.uploaded_utc) FROM backend_uploads u
          WHERE u.device_id=o.device_id AND u.incarnation=o.incarnation
          AND u.sequence=o.sequence)""" if "backend_uploads" in tables else "NULL")
        query = ("""SELECT o.device_id,o.sequence,o.plaintext,r.first_seen_utc,""" +
                 upload_expr + """ FROM verified_observations o
                   LEFT JOIN edge_receipts r USING(device_id,incarnation,sequence)
                   WHERE o.family=1""")
        reports = defaultdict(list)
        for dev, seq, p, first, uploaded in db.execute(query):
            p = bytes(p)
            if len(p) != 64:
                continue
            measured = int.from_bytes(p[16:20], "big") if (p[1] & 1) else None
            if not any(t and START<=t<END for t in (measured, first, uploaded)):
                continue
            reports[bytes(dev).hex().upper()].append(
                dict(seq=seq, t=measured, first=first, upload=uploaded,
                     duration=int.from_bytes(p[20:24],"big"), fix=bool(p[1]&2)))
    for dev, rows in sorted(reports.items()):
        print("\n--- TASMA",dev,"| sunucuda dogrulanmis rapor",len(rows),"---")
        print("  GPS saati var:", sum(x["t"] is not None for x in rows),
              "| konum var:",sum(x["fix"] for x in rows))
        print("  Rapor suresi sn (en sik):",Counter(x["duration"] for x in rows).most_common(7))
        top_gaps([(x["t"], "seq=%s" % x["seq"]) for x in rows if x["t"]],
                 "Gercek olcum saatleri arasindaki bosluklar", 12)
        delays([(x["first"]-x["t"],"seq=%s olcum=%s Edge=%s sunucu=%s" %
                 (x["seq"],stamp(x["t"]),stamp(x["first"]),stamp(x["upload"])))
                for x in rows if x["t"] and x["first"] and x["first"]>=x["t"]],
               "Olcum zamani -> ilk Edge gorusu", 12)
        delays([(x["upload"]-x["first"],"seq=%s Edge=%s sunucu=%s" %
                 (x["seq"],stamp(x["first"]),stamp(x["upload"])))
                for x in rows if x["first"] and x["upload"] and x["upload"]>=x["first"]],
               "Edge gorusu -> sunucu ilk kaydi", 12)
print("\n=== PC EDGE SERVIS HATALARI / AYRILMALAR (21 KASIM) ===")
try:
    p=subprocess.run(["journalctl","--user","-u","orun-pc-edge.service",
                      "--since","2026-11-21 00:00:00","--until","2026-11-22 00:00:00",
                      "-n","3000","--no-pager","-o","short-iso"],
                     capture_output=True,text=True,timeout=20)
    found=[x for x in p.stdout.splitlines()
           if re.search(r"reject|error|failed|timeout|disconnec|reconnect|backend.*(confirmed|retry)",x,re.I)]
    for line in found[-35:]:
        print(line[:260])
    if p.returncode:
        print("Gunluk okunamadi:",p.stderr[:180])
except (OSError,subprocess.TimeoutExpired):
    print("Gunluk okunamadi.")
print("\nBITTI: Sunucu, PC Edge, APK, firmware, kayitlar DEGISMEDI.")
