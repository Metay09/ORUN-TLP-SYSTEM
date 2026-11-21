#!/usr/bin/env python3
"""ORUN: two-collar LoRa collision hypothesis, passive interval snapshot.

Reads PC Edge SQLite and existing journal only. Does NOT open serial ports,
touch services, send RF, change device settings, or write database files.
"""
from __future__ import annotations
import argparse
from collections import Counter
from datetime import datetime
from pathlib import Path
import re
import sqlite3
import subprocess
import time

DEVICES = {
    "0E8ADE7E71531AA3": "ORUN-1",
    "52E16773141A8887": "Kutu",
}
PC_DB = Path.home() / ".local/share/orun/pc-edge-inbox.sqlite"
SERVER_DB = Path.home() / "orun-backend-bench.sqlite"

def ts(t):
    return datetime.fromtimestamp(t).strftime("%H:%M:%S") if t is not None else "-"

def read_db(path):
    if not path.is_file():
        return None
    return sqlite3.connect(path.resolve().as_uri() + "?mode=ro", uri=True, timeout=7)

def median(values):
    values = sorted(values)
    if not values:
        return "-"
    return "%.1fs" % values[len(values)//2]

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--minutes", type=int, default=45)
    args = parser.parse_args()
    if not 10 <= args.minutes <= 180:
        raise SystemExit("minutes: 10..180")
    end = int(time.time())
    start = end - args.minutes*60
    print("=== ORUN PASIF IKI TASMA / RF KARSILASTIRMA ===")
    print("Pencere: %s - %s (%d dk)" %
          (datetime.fromtimestamp(start), datetime.fromtimestamp(end), args.minutes))
    print("Radyo TX/RX sayaci degil, PC Edge'e GELEN paketler sayilir.")

    conn = read_db(PC_DB)
    if conn is None:
        raise SystemExit("DUR: mevcut PC Edge DB bulunamadi: " + str(PC_DB))
    try:
        rows = conn.execute(
            "SELECT id,frame,first_seen_utc,backend_state,last_attempt_utc FROM sealed_records "
            "WHERE first_seen_utc>=? AND first_seen_utc<=? ORDER BY first_seen_utc,id",
            (start,end)).fetchall()
        pending = conn.execute(
            "SELECT count(*) FROM sealed_records WHERE backend_state IS NULL").fetchone()[0]
        bad = conn.execute(
            "SELECT count(*) FROM sealed_records WHERE backend_state IN ('CONFLICT','REJECTED')").fetchone()[0]
    finally:
        conn.close()
    print("PC Edge simdi: %d PENDING / %d REJECTED+CONFLICT" % (pending,bad))
    data = {x: [] for x in DEVICES}
    for rid, frame, first, state, ack in rows:
        frame = bytes(frame)
        if len(frame) not in (84,100):
            continue
        dev = frame[8:16].hex().upper()
        if dev not in data:
            continue
        data[dev].append(dict(id=rid,frame=frame,seen=first,
            mode=frame[4], state=state or "PENDING",ack=ack))
    # Only accepted exact frames can be paired with GPS measurement timestamps.
    measured = {}
    conn = read_db(SERVER_DB)
    if conn is not None:
        try:
            for frame, plaintext in conn.execute(
              "SELECT exact_first_frame,plaintext FROM verified_observations WHERE family=1"):
                p = bytes(plaintext)
                if len(p)==64 and p[1]&1:
                    measured[bytes(frame)] = int.from_bytes(p[16:20],"big")
        except sqlite3.Error as exc:
            print("Sunucu olcum saati okunamadi:",type(exc).__name__)
        finally:
            conn.close()
    for dev, name in DEVICES.items():
        vals = data[dev]
        print("\n%s %s | %d alinan paket" % (name,dev,len(vals)))
        print("  Mod 00 (canli)=%d / Mod 03 (kalici)=%d" %
              (sum(r["mode"]==0 for r in vals), sum(r["mode"]==3 for r in vals)))
        print("  Sunucu sonucu:", dict(Counter(r["state"] for r in vals)))
        live = [r for r in vals if r["mode"]==0]
        live_gaps = [(b["seen"]-a["seen"],ts(a["seen"])+" -> "+ts(b["seen"]))
                     for a,b in zip(live,live[1:])]
        if live_gaps:
            print("  Canli paket araligi ortanca: %s; en uzun: %s" %
                  (median([g for g,_ in live_gaps]),str(max(live_gaps))))
        else:
            print("  Canli paket araligi olculemiyor (0 veya 1 paket).")
        fresh = []
        stale = []
        for r in vals:
            t = measured.get(r["frame"])
            if t and r["seen"] >= t:
                age = r["seen"]-t
                (stale if age>180 else fresh).append((age, r))
        print("  Olcum saati eslesen: taze(<=180sn)=%d, gec(>180sn)=%d" %
              (len(fresh),len(stale)))
        if stale:
            print("  En gec gelen 3:")
            for age,r in sorted(stale,reverse=True,key=lambda x:x[0])[:3]:
                print("    ID %s mod=%s: olcum %s Edge %s gecikme %.1f dk" %
                      (r["id"],r["mode"],ts(r["seen"]-age),ts(r["seen"]),age/60))
        print("  Son alinan:", " / ".join(
          "%s mod%s %s" % (ts(r["seen"]),r["mode"],r["state"])
          for r in vals[-3:]) if vals else "YOK")
        ticks=Counter(datetime.fromtimestamp(r["seen"]).strftime("%H:%M")
                      for r in live)
        print("  Canli gelis dakikalari:", ", ".join(
          "%s(%d)" % (k,v) for k,v in sorted(ticks.items())[-18:]) or "YOK")
    print("\n=== PC EDGE / GATEWAY MEVCUT GUNLUK ===")
    try:
        cmd=["journalctl","--user","-u","orun-pc-edge.service",
             "--since",datetime.fromtimestamp(start).strftime("%Y-%m-%d %H:%M:%S"),
             "--until",datetime.fromtimestamp(end).strftime("%Y-%m-%d %H:%M:%S"),
             "--no-pager","-o","cat","-n","8000"]
        p=subprocess.run(cmd,text=True,capture_output=True,timeout=12)
        lines=p.stdout.splitlines()
        print("Gunluk satiri:",len(lines))
        counters=[]
        for line in lines:
            if line.startswith("[gateway]") and "custody_started=" in line:
                d={k:int(v) for k,v in re.findall(
                    r"(custody_started|duplicates|denied|ack_tx|handed_off|reclaimed)=(\d+)",
                    line)}
                if d:
                    counters.append(d)
        if len(counters)>=2:
            a,b=counters[0],counters[-1]
            print("Gateway BENCH sayac farklari (pencere, reboot yoksa):")
            for key in ("custody_started","duplicates","denied","ack_tx","handed_off","reclaimed"):
                if key in a and key in b:
                    n=b[key]-a[key]
                    print("  %s: %s" %(key,n if n>=0 else "SAYAC RESETLENDI"))
        else:
            print("Gateway BENCH sayaclari bu aralikta yetersiz.")
        errs=[l for l in lines if re.search(
            r"\[usb\].*(disconnected|reconnect)|\[backend\].*(error|failed|timeout)|"
            r"\[gateway\].*(fail|error)",l,re.I)]
        print("USB/backend hata satiri:",len(errs))
        for line in errs[-8:]:
            print(" ",line[:180])
        if p.returncode:
            print("Gunluk erisimi sinirli:",p.stderr[:150])
    except (OSError,subprocess.TimeoutExpired) as exc:
        print("Gunluk okunamadi:",type(exc).__name__)
    print("\nUYARI: Paket cakismasi RF RX/hata bilgisi olmadan kesinlesmez.")
    print("SON: salt okunur; servis/radyo/uygulama/firmware degismedi.")

if __name__=="__main__":
    main()
