#!/usr/bin/env python3
"""Y1: USB'deki ORUN cihazının yazılım yüklemeden önce ve sonra durumu.

  python3 tools/orun_cihaz_kontrol.py oku --port P --out once.json
  python3 tools/orun_cihaz_kontrol.py karsilastir once.json sonra.json

"oku" cihaza yalnız okuma soruları sorar (BENCH?, PRODUCT STORE?, APP DEVICE?);
hiçbir şey yazmaz, silmez. Anahtarın kendisi hiçbir zaman okunmaz ya da
yazılmaz: yalnız kimliği (credential id) ve dönemi (epoch) karşılaştırılır.
"karsilastir" yükleme sonrası cihazın aynı cihaz olduğunu, anahtarının ve
kayıtlarının yerinde durduğunu doğrular.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
import time

TRACKER = re.compile(r"BENCH TRACKER id=([0-9A-F]{16}) .*?granted_gateway=([0-9A-F]{16}|none)"
                     r".*?product_store=(\w+)")
CREDENTIAL = re.compile(r"BENCH CREDENTIAL credential=([0-9A-F]{32}|none) epoch=([0-9A-F]{8}) "
                        r"grant_generation=([0-9A-F]{8}) custody_floor=([0-9A-F]{8})")
GATEWAY = re.compile(r"BENCH GATEWAY id=([0-9A-F]{16}) custody_region=(\w+)"
                     r"(?: custody_ready=(\w+) held=(-?\d+)/(\d+))?")
OGK1 = re.compile(r"BENCH OGK1 status=(\w+) generation=([0-9A-F]{16})")
STORE = re.compile(r"PRODUCT STORE ready=(\w+) faulted=(\w+) inc=(\w+) next_seq=(\d+) retained=(-?\d+)")
DEVICE = re.compile(r"APP DEVICE id=\d+ fw=(\S+)")


def parse(lines: list[str]) -> dict:
    """What the device told about itself; only public facts, never a key."""
    out: dict = {"kind": None}
    for line in lines:
        line = line.strip()
        if m := TRACKER.search(line):
            out.update(kind="tracker", id=m[1], granted=m[2], product_store=m[3])
        elif m := CREDENTIAL.search(line):
            out.update(credential=m[1], epoch=m[2], grant_generation=m[3], custody_floor=m[4])
        elif m := GATEWAY.search(line):
            out.update(kind="gateway", id=m[1], custody_region=m[2])
            if m[4] is not None:
                out.update(custody_ready=m[3], held=int(m[4]), capacity=int(m[5]))
        elif m := OGK1.search(line):
            out.update(ogk1=m[1], ogk1_generation=m[2])
        elif m := STORE.search(line):
            out.update(store_ready=m[1], store_faulted=m[2], store_inc=m[3],
                       next_seq=int(m[4]), retained=int(m[5]))
        elif m := DEVICE.search(line):
            out.update(fw=m[1])
    return out


def complete(info: dict) -> bool:
    """Did the device tell everything the comparison needs?

    Right after a restart the tracker's record store may still be busy for a
    few seconds; it then prints next_seq=0 ("not known yet"), which is not a
    real sequence. Such a reading is asked again rather than compared.
    """
    if not info.get("kind"):
        return False
    if info["kind"] == "tracker" and info.get("store_inc") and info.get("next_seq", 0) == 0:
        return False
    return True


def compare(before: dict, after: dict) -> list[tuple[bool, str]]:
    """(ok, Turkish line) for each thing that must have survived the update."""
    res: list[tuple[bool, str]] = []

    def need(ok: bool, text: str) -> None:
        res.append((bool(ok), text))

    need(after.get("kind") == before.get("kind"), f"Cihaz türü aynı ({after.get('kind')})")
    need(after.get("id") == before.get("id"), f"Cihaz kimliği aynı ({after.get('id')})")
    if before.get("kind") == "tracker":
        need(after.get("credential") == before.get("credential") and after.get("epoch") == before.get("epoch"),
             "Tasmanın anahtarı yerinde (kimlik ve dönem aynı)")
        need(after.get("granted") == before.get("granted") and
             after.get("grant_generation") == before.get("grant_generation"),
             "Gateway izni yerinde")
        if before.get("store_inc"):
            need(after.get("store_inc") == before.get("store_inc"), "Kayıt hafızası aynı (silinmedi)")
            need(after.get("store_ready") == "yes" and after.get("store_faulted") == "no", "Kayıt hafızası hazır")
            b, a_ = before.get("next_seq", 0), after.get("next_seq", 0)
            if a_ == 0:
                need(False, f"Kayıt sırası okunamadı (hafıza meşgul; önce {b})")
            else:
                need(a_ >= b, f"Kayıt sırası geri gitmedi ({b} → {a_})")
    elif before.get("kind") == "gateway":
        need(after.get("custody_region") == "READY" or after.get("custody_region") == before.get("custody_region"),
             f"Gateway belleği hazır ({after.get('custody_region')})")
        if before.get("held") is not None:
            need(after.get("held", -1) >= before.get("held", 0),
                 f"Gateway'deki kayıtlar yerinde ({after.get('held')}/{after.get('capacity')})")
        if before.get("ogk1_generation"):
            need(after.get("ogk1_generation") == before.get("ogk1_generation"), "Gateway'in imzalı izni yerinde")
    if after.get("fw"):
        res.append((True, f"Yazılım: {after['fw']}" + ("" if after.get("fw") != before.get("fw") else " (öncekiyle aynı)")))
    return res


def read(port: str, seconds: float = 6.0) -> list[str]:
    import serial  # pyserial; PC'de kurulu (eski araçlar da kullanıyor)
    lines: list[str] = []
    with serial.Serial(port, 115200, timeout=0.25, write_timeout=2, exclusive=True) as ser:
        try:
            ser.rts = False
            ser.dtr = True   # USB CDC: cihaz konuşmaya başlasın
        except OSError:
            pass
        time.sleep(0.8)
        ser.reset_input_buffer()
        for cmd in ("BENCH?", "PRODUCT STORE?", "APP DEVICE?"):
            ser.write((cmd + "\n").encode("ascii"))
            ser.flush()
            end = time.monotonic() + seconds / 3
            while time.monotonic() < end:
                text = ser.readline().decode("utf8", "replace").strip()
                if text:
                    lines.append(text)
    return lines


def main() -> int:
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("oku")
    r.add_argument("--port", required=True)
    r.add_argument("--out", required=True)
    r.add_argument("--deneme", type=int, default=1, help="cihaz açılana kadar kaç kez denensin")
    c = sub.add_parser("karsilastir")
    c.add_argument("once")
    c.add_argument("sonra")
    a = ap.parse_args()
    if a.cmd == "oku":
        last, best = "", None
        for i in range(a.deneme):
            try:
                info = parse(read(a.port))
                if info.get("kind"):
                    best = info
                    if complete(info):
                        break
                    last = "kayıt hafızası meşgul"
                else:
                    last = "cihaz kendini tanıtmadı"
            except Exception as exc:  # port henüz yok / meşgul
                last = str(exc)
            if i + 1 < a.deneme:
                time.sleep(2)
        if best is None:
            print(f"DUR: cihaz okunamadı ({last})", file=sys.stderr)
            return 2
        # Eksik okuma da yazılır; karşılaştırma eksik kalanı SORUN olarak gösterir.
        with open(a.out, "w") as f:
            json.dump(best, f)
        print(f"Cihaz: {'Tasma' if best['kind'] == 'tracker' else 'Gateway'} {best.get('id')}"
              + (f" · yazılım {best['fw']}" if best.get("fw") else ""))
        return 0
    with open(a.once) as f:
        before = json.load(f)
    with open(a.sonra) as f:
        after = json.load(f)
    bad = 0
    for ok, text in compare(before, after):
        print(("  TAMAM  " if ok else "  SORUN  ") + text)
        bad += 0 if ok else 1
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
