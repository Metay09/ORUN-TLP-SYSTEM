#!/usr/bin/env python3
"""21 Nov ORUN Kutu live/no-fix/interval change, read-only and metadata-only."""
from collections import Counter, defaultdict
from datetime import datetime
from pathlib import Path
import sqlite3

P=Path.home()/"orun-backend-bench.sqlite"
E=Path.home()/".local/share/orun/pc-edge-inbox.sqlite"
def f(t):
    return datetime.fromtimestamp(t).strftime("%d.%m %H:%M:%S") if t else "-"
def openro(p):
    return sqlite3.connect(p.resolve(strict=True).as_uri()+"?mode=ro",uri=True,timeout=8)
def u(b):
    return int.from_bytes(b,"big")
def epoch(h,m=0):
    return int(datetime(2026,11,21,h,m).timestamp())
with openro(P) as db:
    rows=db.execute("""SELECT hex(o.device_id),o.sequence,o.plaintext,r.first_seen_utc,
                         (SELECT MIN(u.uploaded_utc) FROM backend_uploads u
                          WHERE u.device_id=o.device_id AND u.incarnation=o.incarnation
                          AND u.sequence=o.sequence)
                       FROM verified_observations o
                       LEFT JOIN edge_receipts r USING(device_id,incarnation,sequence)
                       WHERE o.family=1""").fetchall()
    samples=defaultdict(list)
    for d,seq,p,seen,uploaded in rows:
        p=bytes(p)
        if len(p)!=64: continue
        flags=p[1]
        measure=u(p[16:20]) if flags&1 else None
        if not any(t and epoch(0)<=t<epoch(23,59)+60 for t in (measure,seen,uploaded)): continue
        samples[d].append(dict(seq=seq,seen=seen,upload=uploaded,
                               measure=measure,dur=u(p[20:24]),fix=bool(flags&2),
                               time=bool(flags&1),source=p[2],flags=flags))
for d,rows in sorted(samples.items()):
    rows.sort(key=lambda x: (x["seen"] or x["measure"] or 0,x["seq"]))
    print("\n====",d,"| verified PERIODIC rows",len(rows),"====")
    print("SAAT | RAPOR | GPS_SAAT | GPS_KONUM | SURE(s) EN SIK | ILK EDGE")
    by=defaultdict(list)
    for x in rows:
        moment=x["seen"] or x["measure"]
        if moment: by[datetime.fromtimestamp(moment).hour].append(x)
    for h in sorted(by):
        subset=by[h]
        most=Counter(x["dur"] for x in subset).most_common(3)
        print("%02d | %3d | %3d | %3d | %s | %s"%
              (h,len(subset),sum(x["time"] for x in subset),
               sum(x["fix"] for x in subset),most,
               f(min((x["seen"] for x in subset if x["seen"]),default=None))))
    if d=="52E16773141A8887":
        print("\nKUTU: 01:00-12:50 GPS/rapor kronolojisi (maks 40 satir)")
        subset=[x for x in rows if x["seen"] and epoch(1)<=x["seen"]<=epoch(12,50)]
        if len(subset)>40: subset=subset[:20]+subset[-20:]
        for x in subset:
            print(" seq=%-5s Edge=%s Olcum=%s per=%4ds GPS=%s saat=%s server=%s"%
                  (x["seq"],f(x["seen"]),f(x["measure"]),x["dur"],
                   "VAR" if x["fix"] else "YOK",
                   "VAR" if x["time"] else "YOK",f(x["upload"])))
    else:
        print("\nORUN-1: Olcum->Edge gecikmesi 15 dk ustu")
        for x in sorted(rows,key=lambda x:(x["seen"]-x["measure"]) if x["seen"] and x["measure"] else -1,reverse=True):
            if not x["seen"] or not x["measure"] or x["seen"]-x["measure"]<900: continue
            print(" seq=%-4s olcum=%s Edge=%s +%4.1f dk"%
                  (x["seq"],f(x["measure"]),f(x["seen"]),(x["seen"]-x["measure"])/60))
with openro(E) as db:
    print("\n==== KUTU PC EDGE: 00 ve 03 mod saatlik ====")
    buckets=defaultdict(Counter)
    for b,seen in db.execute("SELECT frame,first_seen_utc FROM sealed_records WHERE first_seen_utc>=? AND first_seen_utc<?",
                            (epoch(0),epoch(23,59)+60)):
        b=bytes(b)
        if len(b)!=100 or b[8:16].hex().upper()!="52E16773141A8887":continue
        buckets[datetime.fromtimestamp(seen).hour][b[4]]+=1
    for h in sorted(buckets):
        print(" %02d:00 | canli/00=%3d | makbuzlu/03=%3d"%
              (h,buckets[h][0],buckets[h][3]))
print("\nSalt okunur; kayit, servis, firmware degismedi.")
