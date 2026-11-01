#!/usr/bin/env python3
"""Explicit local USB geofence writer for an ORUN TRACKER (never a Gateway).

Reads the phone's GeoJSON export and validates the whole E7 geometry before any
serial I/O. This is physical-access USB only, not a remote/BLE security path.
No CLEAR is ever inferred from an empty file. Never claims success from a write.
"""
from __future__ import annotations

import argparse
from decimal import Decimal, ROUND_FLOOR
import json
from pathlib import Path
import re
import time

SCALE = Decimal(10_000_000)
RESULT = re.compile(
    r"^APP FENCE id=\d+ code=(\w+) state=(\w+) areas=(\d+) "
    r"vertices=(\d+) token=(\w+)"
)
STAGED = re.compile(r"^APP FENCE STAGING areas=(\d+) vertices=(\d+)$")
STATUS = re.compile(
    r"^APP GEOFENCE id=\d+ resource=\d+ token=\d+ "
    r"areas=(\d+) vertices=(\d+) runtime=(\w+)"
)


class FenceError(Exception):
    pass


def as_e7(x: object) -> int:
    if isinstance(x, bool) or not isinstance(x, (int, float)):
        raise FenceError("Koordinat sayi olmali")
    try:
        y = Decimal(str(x)) * SCALE
        if not y.is_finite():
            raise ValueError()
        # JavaScript Math.round semantics, including negative half ties.
        return int((y + Decimal("0.5")).to_integral_value(rounding=ROUND_FLOOR))
    except (ValueError, ArithmeticError) as exc:
        raise FenceError("Gecersiz koordinat") from exc


def orientation(a: tuple[int,int], b: tuple[int,int], p: tuple[int,int]) -> int:
    return (b[1]-a[1])*(p[0]-a[0])-(b[0]-a[0])*(p[1]-a[1])


def intersects(a, b, c, d) -> bool:
    o1, o2, o3, o4 = orientation(a,b,c), orientation(a,b,d), orientation(c,d,a), orientation(c,d,b)
    if (o1 > 0) != (o2 > 0) and (o3 > 0) != (o4 > 0) and o1*o2 < 0 and o3*o4 < 0:
        return True
    def between(p,q,r):
        return min(p[0],r[0]) <= q[0] <= max(p[0],r[0]) and min(p[1],r[1]) <= q[1] <= max(p[1],r[1])
    return ((o1 == 0 and between(a,c,b)) or (o2 == 0 and between(a,d,b)) or
            (o3 == 0 and between(c,a,d)) or (o4 == 0 and between(c,b,d)))


def checked_ring(points: object) -> list[tuple[int,int]]:
    if not isinstance(points, list) or not 3 <= len(points) <= 65:
        raise FenceError("Alan 3-64 kose icermeli")
    vertices = []
    for point in points:
        if not isinstance(point, list) or len(point) != 2:
            raise FenceError("GeoJSON koordinati [lon,lat] olmali")
        lon, lat = as_e7(point[0]), as_e7(point[1])
        if not -900_000_000 < lat < 900_000_000 or not -1_800_000_000 < lon < 1_800_000_000:
            raise FenceError("Koordinat alan disinda")
        vertices.append((lat, lon))
    if len(vertices) > 1 and vertices[0] == vertices[-1]:
        vertices.pop()
    n = len(vertices)
    if not 3 <= n <= 64:
        raise FenceError("Etkin kose siniri asildi")
    if (max(p[0] for p in vertices)-min(p[0] for p in vertices) > 100_000_000 or
            max(p[1] for p in vertices)-min(p[1] for p in vertices) > 100_000_000):
        raise FenceError("Alan 10 dereceden genis")
    for i in range(n):
        if vertices[i] == vertices[(i+1) % n]:
            raise FenceError("Ayni komsu kose")
    ax, ay = vertices[0][1], vertices[0][0]
    twice = sum((a[1]-ax)*(b[0]-ay)-(a[0]-ay)*(b[1]-ax)
                 for a,b in zip(vertices,vertices[1:]+vertices[:1]))
    if twice == 0:
        raise FenceError("Sifir alanli cokgen")
    for i in range(n):
        for j in range(i+1,n):
            if j == i+1 or (i == 0 and j == n-1):
                continue
            if intersects(vertices[i],vertices[(i+1)%n],vertices[j],vertices[(j+1)%n]):
                raise FenceError("Kesisen kenarlar")
    return vertices


def parse_geojson(data: object) -> list[list[tuple[int,int]]]:
    if not isinstance(data, dict) or data.get("type") != "FeatureCollection" or not isinstance(data.get("features"), list):
        raise FenceError("Telefonun GeoJSON FeatureCollection dosyasi gerekli")
    result: list[list[tuple[int,int]]] = []
    for f in data["features"]:
        if not isinstance(f,dict) or f.get("type") != "Feature":
            raise FenceError("Gecersiz GeoJSON feature")
        props = f.get("properties") or {}
        if not isinstance(props,dict) or type(props.get("active",True)) is not bool:
            raise FenceError("active alani boolean olmali")
        if not props.get("active",True):
            continue
        geometry = f.get("geometry")
        if not isinstance(geometry,dict) or geometry.get("type") != "Polygon":
            raise FenceError("Yalniz deliksiz Polygon desteklenir")
        rings = geometry.get("coordinates")
        if not isinstance(rings,list) or len(rings) != 1:
            raise FenceError("Delikli ya da bozuk cokgeni tasmaya gonderme")
        result.append(checked_ring(rings[0]))
    if not 1 <= len(result) <= 8 or sum(map(len,result)) > 64:
        raise FenceError("Tasma 1-8 aktif alan ve toplam en fazla 64 kose destekler")
    return result


def decimal_e7(n: int) -> str:
    return str((Decimal(n) / SCALE).quantize(Decimal("0.0000001")))


def commands(polygons: list[list[tuple[int,int]]]) -> list[tuple[str,int,int]]:
    out = [("APP FENCE BEGIN",1,0)]
    total = 0
    for i,poly in enumerate(polygons):
        if i:
            out.append(("APP FENCE AREA",i+1,total))
        for lat,lon in poly:
            total += 1
            out.append((f"APP FENCE V {decimal_e7(lat)} {decimal_e7(lon)}",i+1,total))
    return out


def wait_line(ser, match, seconds: float) -> str:
    until = time.monotonic() + seconds
    while time.monotonic() < until:
        line = ser.readline().decode("ascii", "replace").strip()
        if not line:
            continue
        if line.startswith(("APP FENCE REJECTED", "APP REJECTED", "APP BUSY")):
            raise FenceError("Cihaz reddetti: "+line)
        if match(line):
            return line
    raise FenceError("Cihazdan dogrulama gelmedi; islem sonucunu varsaymayin")


def apply_usb(port: str, expected_id: str, stages: list[tuple[str,int,int]], n: int, v: int) -> None:
    if not re.fullmatch(r"[0-9A-F]{16}",expected_id):
        raise FenceError("Board ID 16 buyuk hex karakter olmali")
    real = Path(port)
    if (not str(real).startswith("/dev/serial/by-id/") or
            f"Board_{expected_id}-if00" not in real.name or not real.exists()):
        raise FenceError("USB symlink/board kimligi eslesmiyor")
    try:
        import serial
    except ImportError as exc:
        raise FenceError("pyserial kurulu degil") from exc
    with serial.Serial(port, 115200, timeout=0.3, write_timeout=2, exclusive=True) as ser:
        ser.dtr = True
        time.sleep(0.5)
        def send(text: str):
            ser.reset_input_buffer()
            ser.write((text+"\n").encode("ascii"))
            ser.flush()
        send("APP GEOFENCE?")
        wait_line(ser, lambda s: STATUS.fullmatch(s) is not None, 8)
        committed = False
        try:
            for line, area, vertices in stages:
                send(line)
                wait_line(ser, lambda s: (m:=STAGED.fullmatch(s)) is not None and
                          int(m[1]) == area and int(m[2]) == vertices, 5)
            committed = True
            send("APP FENCE COMMIT")
            result = wait_line(ser, lambda s: RESULT.fullmatch(s) is not None, 25)
            m = RESULT.fullmatch(result)
            assert m is not None
            if (m[1] not in ("APPLIED","UNCHANGED") or m[2] != "CONFIGURED" or
                    int(m[3]) != n or int(m[4]) != v or m[5] != "VALID"):
                raise FenceError("Kesin basari yok: "+result)
            send("APP GEOFENCE?")
            observed = wait_line(ser, lambda s: STATUS.fullmatch(s) is not None, 8)
            status = STATUS.fullmatch(observed)
            assert status is not None
            if int(status[1]) != n or int(status[2]) != v or status[3] != "configured":
                raise FenceError("Durum yeniden okuma uyusmuyor: "+observed)
            print("CIHAZ YANITI:",result)
            print("DURUM:",observed)
            print("USB basvuru/sonuc ve adetler dogrulandi; geometri readback/digest yok.")
        except Exception:
            if not committed:
                try:
                    ser.write(b"APP FENCE ABORT\n")
                except Exception:
                    pass
            raise


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("geojson",type=Path,help="Android uygulamasindan disari aktarilan tam alan listesi")
    ap.add_argument("--port",help="Tracker /dev/serial/by-id/... yolu; Gateway degil")
    ap.add_argument("--expected-board-id",help="Tracker USB kimligi (16 buyuk hex)")
    ap.add_argument("--apply",action="store_true",help="USB yazma: fiziksel yetki gerektirir")
    args = ap.parse_args()
    try:
        polygons = parse_geojson(json.loads(args.geojson.read_text(encoding="utf-8")))
        stages = commands(polygons)
        vertices = sum(map(len,polygons))
        print("TASLAK:",len(polygons),"alan,",vertices,"kose")
        print("APP FENCE COMMIT sadece --apply ile USB'ye gonderilir.")
        if not args.apply:
            for line,_,_ in stages:
                print(line)
            return 0
        if not args.port or not args.expected_board_id:
            raise FenceError("--apply icin --port ve --expected-board-id zorunlu")
        apply_usb(args.port,args.expected_board_id,stages,len(polygons),vertices)
        return 0
    except (OSError, ValueError, FenceError) as exc:
        print("DUR:",exc)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
